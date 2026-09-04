/*
 * native_daochi.c - Daochi node client for krait.
 *
 * Accounts are ML-DSA-44 keypairs owned by kryon's ksync layer; login runs
 * the challenge/sign flow and caches the bearer token in the state file.
 * Friends and shared boards wrap the node's JSON API through
 * RequestKsyncSyncBearer. Everything is synchronous: calls are short lived
 * (sub-second on a healthy node) and the status string tells the UI what
 * happened. The board cache is rebuilt by the refresh calls; index-based
 * accessors keep .kry code free of JSON.
 */
#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include "ksync_account.h"
#include "ksync_sync.h"
#include "kry_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DC_DEFAULT_NODE_URL "https://api.waozi.xyz"
#define DC_MAX_FRIENDS      128
#define DC_MAX_REQUESTS     64
#define DC_MAX_BOARDS       64
#define DC_MAX_MEMBERS      64
#define DC_MAX_RECORDS      512
#define DC_PAYLOAD_MAX      8192

typedef struct {
    char id[65];
    char alias[64];
} DcFriend;

typedef struct {
    char id[65];
    char requester_alias[64];
    char target_alias[64];
    int incoming;
} DcFriendRequest;

typedef struct {
    char id[40];
    char title[128];
    char my_permission[8];   /* owner | write | read */
    int member_count;
    int record_count;
} DcBoard;

typedef struct {
    char id[65];
    char alias[64];
    char permission[8];
} DcMember;

typedef struct {
    char id[128];
    long long updated_at;
    int deleted;
    char payload[DC_PAYLOAD_MAX];
} DcRecord;

static struct {
    KsyncAccount account;
    int account_loaded;
    DcFriend friends[DC_MAX_FRIENDS];
    int friend_count;
    DcFriendRequest requests[DC_MAX_REQUESTS];
    int request_count;
    DcBoard boards[DC_MAX_BOARDS];
    int board_count;
    int selected_board;      /* index into boards, -1 none */
    DcMember members[DC_MAX_MEMBERS];
    int member_count;
    DcRecord records[DC_MAX_RECORDS];
    int record_count;
    char node_name[64];
    char status[192];
} g_dc;

static char g_dc_token_scratch[4096];

/* ------------------------------------------------------------------ */
/* state file (key = value lines)                                      */
/* ------------------------------------------------------------------ */

static void
dc_dir(char *dst, size_t dst_size)
{
    const char *home = getenv("HOME");

    if(home == NULL || home[0] == '\0')
        snprintf(dst, dst_size, ".kryon/krait/daochi");
    else
        snprintf(dst, dst_size, "%s/.kryon/krait/daochi", home);
}

/* mkdir -p for the daochi tree: krait_ensure_parent_dir is single-level */
static void
dc_ensure_dirs(const char *path)
{
    char partial[KRAIT_PATH_MAX];
    size_t len;

    snprintf(partial, sizeof(partial), "%s", path);
    len = strlen(partial);
    while(len > 0 && partial[len - 1] == '/')
        partial[--len] = '\0';
    for(size_t i = 1; i < len; i++) {
        if(partial[i] == '/') {
            partial[i] = '\0';
            mkdir(partial, 0755);
            partial[i] = '/';
        }
    }
    if(len > 0)
        mkdir(partial, 0755);
}

static void
dc_state_path(char *dst, size_t dst_size)
{
    char dir[KRAIT_PATH_MAX];

    dc_dir(dir, sizeof(dir));
    snprintf(dst, dst_size, "%s/state.conf", dir);
}

static void
dc_account_path(char *dst, size_t dst_size)
{
    char dir[KRAIT_PATH_MAX];

    dc_dir(dir, sizeof(dir));
    snprintf(dst, dst_size, "%s/account.txt", dir);
}

/* strip trailing newline in place */
static void
dc_chomp(char *s)
{
    size_t n = strlen(s);

    while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static int
dc_state_get(const char *key, char *out, size_t out_size)
{
    char path[KRAIT_PATH_MAX];
    FILE *f;
    size_t key_len = strlen(key);
    int found = 0;

    dc_state_path(path, sizeof(path));
    f = fopen(path, "r");
    if(f == NULL)
        return 0;
    while(fgets(out, (int)out_size, f) != NULL) {
        dc_chomp(out);
        if(strncmp(out, key, key_len) == 0 && out[key_len] == '=') {
            memmove(out, out + key_len + 1, strlen(out + key_len + 1) + 1);
            found = 1;
            break;
        }
    }
    fclose(f);
    if(!found)
        out[0] = '\0';
    return found;
}

static void
dc_state_set(const char *key, const char *value)
{
    char path[KRAIT_PATH_MAX];
    char tmp[KRAIT_PATH_MAX];
    FILE *out;
    char line[512];

    dc_state_path(path, sizeof(path));
    {
        char dir[KRAIT_PATH_MAX];

        dc_dir(dir, sizeof(dir));
        dc_ensure_dirs(dir);
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    out = fopen(tmp, "w");
    if(out == NULL)
        return;
    {
        FILE *in = fopen(path, "r");
        size_t key_len = strlen(key);

        if(in != NULL) {
            while(fgets(line, sizeof(line), in) != NULL) {
                dc_chomp(line);
                if(strncmp(line, key, key_len) == 0 && line[key_len] == '=')
                    continue;
                if(line[0] != '\0')
                    fprintf(out, "%s\n", line);
            }
            fclose(in);
        }
    }
    if(value != NULL && value[0] != '\0')
        fprintf(out, "%s=%s\n", key, value);
    fclose(out);
    chmod(tmp, 0600);
    rename(tmp, path);
}

/* ------------------------------------------------------------------ */
/* account                                                             */
/* ------------------------------------------------------------------ */

static void
dc_load_account(void)
{
    char path[KRAIT_PATH_MAX];
    char *text = NULL;
    long len = 0;

    if(g_dc.account_loaded)
        return;
    g_dc.account_loaded = 1;
    memset(&g_dc.account, 0, sizeof(g_dc.account));
    dc_account_path(path, sizeof(path));
    if(!krait_read_file_alloc(path, &text, &len) || text == NULL)
        return;
    if(!ParseKsyncAccountText(text, &g_dc.account))
        memset(&g_dc.account, 0, sizeof(g_dc.account));
    free(text);
}

static void
dc_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g_dc.status, sizeof(g_dc.status), fmt, ap);
    va_end(ap);
}

static char g_dc_node_url_cache[256];

static const char *
dc_node_url(void)
{
    if(g_dc_node_url_cache[0] == '\0') {
        dc_state_get("node_url", g_dc_node_url_cache, sizeof(g_dc_node_url_cache));
        if(g_dc_node_url_cache[0] == '\0')
            snprintf(g_dc_node_url_cache, sizeof(g_dc_node_url_cache), "%s",
                     DC_DEFAULT_NODE_URL);
    }
    return g_dc_node_url_cache;
}

static const char *
dc_state_get_text(const char *key, void *user)
{
    (void)user;
    dc_state_get(key, g_dc_token_scratch, sizeof(g_dc_token_scratch));
    return g_dc_token_scratch;
}

static void
dc_state_set_text(const char *key, const char *value, void *user)
{
    (void)user;
    dc_state_set(key, value);
}

static void
dc_log_http_failure(const char *step, long status, const char *response,
                    void *user)
{
    (void)user;
    dc_status("%s: HTTP %ld %.120s", step, status,
              response != NULL ? response : "");
    if(getenv("KRAIT_DAOCHI_DEBUG") != NULL)
        fprintf(stderr, "daochi-http: %s status=%ld resp=%.300s\n", step,
                status, response != NULL ? response : "");
}

static KsyncSyncConfig
dc_config(void)
{
    KsyncSyncConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    dc_load_account();
    cfg.base_url = dc_node_url();
    cfg.account = &g_dc.account;
    cfg.client_id = "krait-ide";
    cfg.signature_context = "daochi-sync-v1";
    cfg.user_header_name = "X-Daochi-User";
    cfg.signature_header_name = "X-Daochi-Signature";
    cfg.http_request = KsyncDefaultHttpRequest;
    cfg.get_text = dc_state_get_text;
    cfg.set_text = dc_state_set_text;
    cfg.log_http_failure = dc_log_http_failure;
    cfg.retry_max = 2;
    cfg.retry_delay_ms = 250;
    return cfg;
}

/* bearer request with the real HTTP status preserved (201 vs 200 matters
 * for creates). A 401 re-runs the login flow once, mirroring
 * RequestKsyncSyncBearer's recovery. */
static long
dc_request_once(const KsyncSyncConfig *cfg, const char *method,
                const char *path, const char *body, char *out, size_t out_size)
{
    char url[768];
    char token[4096];
    char auth_header[4200];
    char user_header[96];
    const char *headers[3];
    KsyncSyncBuffer response = {0};
    long status = 0;

    if(out != NULL && out_size > 0)
        out[0] = '\0';
    if(!JoinKsyncSyncURL(url, sizeof(url), cfg->base_url, path))
        return 0;
    dc_state_get("sync_auth_token", token, sizeof(token));
    if(token[0] == '\0')
        return 401;
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
    snprintf(user_header, sizeof(user_header), "X-Daochi-User: %s",
             cfg->account->public_id);
    headers[0] = "Content-Type: application/json";
    headers[1] = auth_header;
    headers[2] = user_header;
    if(!cfg->http_request(method, url, body, headers, 3, &response, &status,
                          cfg->user)) {
        dc_status("%s: connection failed", path);
        FreeKsyncSyncBuffer(&response);
        return 0;
    }
    if(out != NULL && out_size > 0)
        snprintf(out, out_size, "%s", response.data != NULL ? response.data : "");
    FreeKsyncSyncBuffer(&response);
    return status;
}

static long
dc_request(const char *method, const char *path, const char *body,
           char *out, size_t out_size)
{
    KsyncSyncConfig cfg = dc_config();
    long status;

    status = dc_request_once(&cfg, method, path, body, out, out_size);
    if(status == 401 && LoginKsyncSync(&cfg) == KSYNC_SYNC_OK)
        status = dc_request_once(&cfg, method, path, body, out, out_size);
    if(getenv("KRAIT_DAOCHI_DEBUG") != NULL)
        fprintf(stderr, "daochi-http: %s %s -> %ld %.200s\n", method, path,
                status, out != NULL ? out : "");
    return status;
}

static const char *
dc_json_string(const KryJson *v, const char *key, char *dst, size_t dst_size)
{
    const char *s;

    if(v == NULL || dst == NULL || dst_size == 0)
        return NULL;
    dst[0] = '\0';
    s = kry_json_string(kry_json_get(v, key));
    if(s != NULL)
        snprintf(dst, dst_size, "%s", s);
    return dst;
}

/* ------------------------------------------------------------------ */
/* account API                                                         */
/* ------------------------------------------------------------------ */

int krait_daochi_available(void)
{
    return IsKsyncAccountAvailable() ? 1 : 0;
}

int krait_daochi_has_account(void)
{
    dc_load_account();
    return HasKsyncAccountValues(&g_dc.account) ? 1 : 0;
}

const char *krait_daochi_public_id(void)
{
    dc_load_account();
    return g_dc.account.public_id;
}

int krait_daochi_account_create(void)
{
    char path[KRAIT_PATH_MAX];
    char text[KSYNC_ACCOUNT_EXPORT_TEXT_SIZE];

    if(!IsKsyncAccountAvailable()) {
        dc_status("account keys unavailable (built without liboqs)");
        return 0;
    }
    memset(&g_dc.account, 0, sizeof(g_dc.account));
    if(!CreateKsyncAccount(&g_dc.account)) {
        dc_status("account create failed: %s", GetKsyncAccountLastError());
        g_dc.account_loaded = 1;
        return 0;
    }
    g_dc.account_loaded = 1;
    if(!ExportKsyncAccountText(&g_dc.account, text, sizeof(text))) {
        dc_status("account export failed");
        return 0;
    }
    dc_account_path(path, sizeof(path));
    {
        char dir[KRAIT_PATH_MAX];

        dc_dir(dir, sizeof(dir));
        dc_ensure_dirs(dir);
    }
    if(!krait_write_text_file(path, text)) {
        dc_status("cannot write %s", path);
        return 0;
    }
    chmod(path, 0600);
    dc_state_set("sync_auth_token", "");
    dc_state_set("sync_auth_token_expires_at", "");
    dc_status("account created: %s", g_dc.account.public_id);
    return 1;
}

int krait_daochi_account_import_file(const char *filename)
{
    char path[KRAIT_PATH_MAX];

    if(!ImportKsyncAccountFile(filename, &g_dc.account)) {
        dc_status("import failed: %s", GetKsyncAccountLastError());
        g_dc.account_loaded = 1;
        return 0;
    }
    g_dc.account_loaded = 1;
    dc_account_path(path, sizeof(path));
    {
        char text[KSYNC_ACCOUNT_EXPORT_TEXT_SIZE];
        char dir[KRAIT_PATH_MAX];

        if(ExportKsyncAccountText(&g_dc.account, text, sizeof(text))) {
            dc_dir(dir, sizeof(dir));
            dc_ensure_dirs(dir);
            krait_write_text_file(path, text);
            chmod(path, 0600);
        }
    }
    dc_state_set("sync_auth_token", "");
    dc_status("account imported: %s", g_dc.account.public_id);
    return 1;
}

int krait_daochi_account_export_file(const char *filename)
{
    dc_load_account();
    if(!HasKsyncAccountValues(&g_dc.account)) {
        dc_status("no account to export");
        return 0;
    }
    if(!ExportKsyncAccountFile(&g_dc.account, filename)) {
        dc_status("export failed: %s", GetKsyncAccountLastError());
        return 0;
    }
    dc_status("account exported to %s", filename);
    return 1;
}

int krait_daochi_logout(void)
{
    dc_state_set("sync_auth_token", "");
    dc_state_set("sync_auth_token_expires_at", "");
    g_dc.friend_count = 0;
    g_dc.request_count = 0;
    g_dc.board_count = 0;
    g_dc.member_count = 0;
    g_dc.record_count = 0;
    dc_status("logged out (account key kept)");
    return 1;
}

const char *krait_daochi_node_url(void)
{
    return dc_node_url();
}

int krait_daochi_set_node_url(const char *url)
{
    char normalized[256];

    if(url == NULL || url[0] == '\0') {
        dc_state_set("node_url", "");
        g_dc_node_url_cache[0] = '\0';
        dc_status("node url reset to %s", DC_DEFAULT_NODE_URL);
        return 1;
    }
    if(!NormalizeKsyncSyncURL(url, normalized, sizeof(normalized))) {
        dc_status("invalid node url");
        return 0;
    }
    dc_state_set("node_url", normalized);
    g_dc_node_url_cache[0] = '\0';
    dc_state_set("sync_auth_token", "");
    dc_status("node url set to %s", normalized);
    return 1;
}

int krait_daochi_login(void)
{
    KsyncSyncConfig cfg = dc_config();
    KsyncSyncResult result;

    if(!HasKsyncAccountValues(&g_dc.account)) {
        dc_status("no account: create or import one first");
        return 0;
    }
    result = LoginKsyncSync(&cfg);
    if(result != KSYNC_SYNC_OK) {
        dc_status("login failed: %s", GetKsyncSyncResultName(result));
        return 0;
    }
    dc_status("logged in to %s", dc_node_url());
    return 1;
}

int krait_daochi_ping(void)
{
    char response[2048];
    long status;

    status = dc_request("GET", "/api/v1/node", NULL, response, sizeof(response));
    if(status == 200) {
        KryJson *root = kry_json_parse(response);

        dc_json_string(root, "name", g_dc.node_name, sizeof(g_dc.node_name));
        kry_json_free(root);
        dc_status("node %s reachable", g_dc.node_name[0] ? g_dc.node_name : "ok");
        return 1;
    }
    return 0;
}

int krait_daochi_set_alias(const char *alias)
{
    KryJsonBuf b = {0};
    char response[1024];
    long status;

    if(alias == NULL || alias[0] == '\0')
        return 0;
    kry_json_buf_raw(&b, "{\"alias\":");
    kry_json_buf_str(&b, alias);
    kry_json_buf_raw(&b, "}");
    status = dc_request("POST", "/api/v1/account/alias",
                        kry_json_buf_finish(&b), response, sizeof(response));
    kry_json_buf_free(&b);
    if(status != 200) {
        if(status == 409)
            dc_status("alias already taken");
        return 0;
    }
    dc_state_set("alias", alias);
    dc_status("alias set to %s", alias);
    return 1;
}

const char *krait_daochi_alias(void)
{
    static char alias[64];

    if(alias[0] == '\0')
        dc_state_get("alias", alias, sizeof(alias));
    return alias;
}

const char *krait_daochi_status(void)
{
    return g_dc.status;
}

/* ------------------------------------------------------------------ */
/* friends                                                             */
/* ------------------------------------------------------------------ */

int krait_daochi_friends_refresh(void)
{
    char response[64 * 1024];
    long status;

    g_dc.friend_count = 0;
    status = dc_request("GET", "/api/v1/friends", NULL, response, sizeof(response));
    if(status != 200)
        return 0;
    {
        KryJson *root = kry_json_parse(response);
        const KryJson *list;

        if(root == NULL) {
            dc_status("friends: bad response");
            return 0;
        }
        list = kry_json_get(root, "friends");
        for(int i = 0; i < kry_json_count(list) && g_dc.friend_count < DC_MAX_FRIENDS; i++) {
            const KryJson *item = kry_json_at(list, i);
            DcFriend *f = &g_dc.friends[g_dc.friend_count++];

            dc_json_string(item, "user_id_hash", f->id, sizeof(f->id));
            dc_json_string(item, "alias", f->alias, sizeof(f->alias));
        }
        kry_json_free(root);
    }
    dc_status("%d friend(s)", g_dc.friend_count);
    return 1;
}

int krait_daochi_friend_count(void) { return g_dc.friend_count; }
const char *krait_daochi_friend_id(int index)
{
    if(index < 0 || index >= g_dc.friend_count) return "";
    return g_dc.friends[index].id;
}
const char *krait_daochi_friend_alias(int index)
{
    if(index < 0 || index >= g_dc.friend_count) return "";
    return g_dc.friends[index].alias;
}

int krait_daochi_friend_requests_refresh(void)
{
    char response[64 * 1024];
    long status;

    g_dc.request_count = 0;
    status = dc_request("GET", "/api/v1/friends/requests", NULL, response, sizeof(response));
    if(status != 200)
        return 0;
    {
        KryJson *root = kry_json_parse(response);

        if(root == NULL)
            return 0;
        for(int pass = 0; pass < 2; pass++) {
            const KryJson *list = kry_json_get(root, pass == 0 ? "incoming" : "outgoing");

            for(int i = 0; i < kry_json_count(list) && g_dc.request_count < DC_MAX_REQUESTS; i++) {
                const KryJson *item = kry_json_at(list, i);
                DcFriendRequest *r = &g_dc.requests[g_dc.request_count++];

                r->incoming = pass == 0;
                dc_json_string(item, "id", r->id, sizeof(r->id));
                dc_json_string(item, pass == 0 ? "requester_alias" : "target_alias",
                               r->requester_alias, sizeof(r->requester_alias));
            }
        }
        kry_json_free(root);
    }
    return 1;
}

int krait_daochi_friend_request_count(void) { return g_dc.request_count; }
int krait_daochi_friend_request_incoming(int index)
{
    if(index < 0 || index >= g_dc.request_count) return 0;
    return g_dc.requests[index].incoming;
}
const char *krait_daochi_friend_request_alias(int index)
{
    if(index < 0 || index >= g_dc.request_count) return "";
    return g_dc.requests[index].requester_alias[0] != '\0'
               ? g_dc.requests[index].requester_alias
               : g_dc.requests[index].id;
}
const char *krait_daochi_friend_request_id(int index)
{
    if(index < 0 || index >= g_dc.request_count) return "";
    return g_dc.requests[index].id;
}

int krait_daochi_friend_request_send(const char *target)
{
    KryJsonBuf b = {0};
    char response[8192];
    long status;

    if(target == NULL || target[0] == '\0')
        return 0;
    kry_json_buf_raw(&b, "{\"target\":");
    kry_json_buf_str(&b, target);
    kry_json_buf_raw(&b, "}");
    status = dc_request("POST", "/api/v1/friends/requests",
                        kry_json_buf_finish(&b), response, sizeof(response));
    kry_json_buf_free(&b);
    if(status == 404)
        dc_status("no such account");
    if(status == 409)
        dc_status("already friends or requested");
    return status == 201;
}

static int
dc_friend_request_action(int index, const char *action)
{
    char path[160];
    char response[4096];
    long status;

    if(index < 0 || index >= g_dc.request_count)
        return 0;
    snprintf(path, sizeof(path), "/api/v1/friends/requests/%s/%s",
             g_dc.requests[index].id, action);
    status = dc_request("POST", path, "{}", response, sizeof(response));
    return status == 200;
}

int krait_daochi_friend_request_accept(int index)
{
    return dc_friend_request_action(index, "accept");
}

int krait_daochi_friend_request_decline(int index)
{
    return dc_friend_request_action(index, "decline");
}

int krait_daochi_friend_remove(int index)
{
    char path[160];
    char response[4096];
    long status;

    if(index < 0 || index >= g_dc.friend_count)
        return 0;
    snprintf(path, sizeof(path), "/api/v1/friends/%s", g_dc.friends[index].id);
    status = dc_request("DELETE", path, NULL, response, sizeof(response));
    return status == 200;
}

/* ------------------------------------------------------------------ */
/* boards                                                              */
/* ------------------------------------------------------------------ */

int krait_daochi_boards_refresh(void)
{
    char response[64 * 1024];
    long status;

    g_dc.board_count = 0;
    g_dc.selected_board = -1;
    g_dc.member_count = 0;
    g_dc.record_count = 0;
    status = dc_request("GET", "/api/v1/boards", NULL, response, sizeof(response));
    if(status != 200)
        return 0;
    {
        KryJson *root = kry_json_parse(response);
        const KryJson *list;

        if(root == NULL)
            return 0;
        list = kry_json_get(root, "boards");
        for(int i = 0; i < kry_json_count(list) && g_dc.board_count < DC_MAX_BOARDS; i++) {
            const KryJson *item = kry_json_at(list, i);
            DcBoard *board = &g_dc.boards[g_dc.board_count++];

            dc_json_string(item, "id", board->id, sizeof(board->id));
            dc_json_string(item, "title", board->title, sizeof(board->title));
            dc_json_string(item, "my_permission", board->my_permission,
                           sizeof(board->my_permission));
            {
                const KryJson *n = kry_json_get(item, "member_count");

                board->member_count = n != NULL ? (int)kry_json_number(n) : 0;
            }
        }
        kry_json_free(root);
    }
    dc_status("%d board(s)", g_dc.board_count);
    return 1;
}

int krait_daochi_board_count(void) { return g_dc.board_count; }
const char *krait_daochi_board_id(int index)
{
    if(index < 0 || index >= g_dc.board_count) return "";
    return g_dc.boards[index].id;
}
const char *krait_daochi_board_title(int index)
{
    if(index < 0 || index >= g_dc.board_count) return "";
    return g_dc.boards[index].title;
}
const char *krait_daochi_board_permission(int index)
{
    if(index < 0 || index >= g_dc.board_count) return "";
    return g_dc.boards[index].my_permission;
}

int krait_daochi_board_create(const char *title)
{
    KryJsonBuf b = {0};
    char response[8192];
    long status;

    if(title == NULL || title[0] == '\0')
        return 0;
    kry_json_buf_raw(&b, "{\"title\":");
    kry_json_buf_str(&b, title);
    kry_json_buf_raw(&b, ",\"app_id\":\"krait\"}");
    status = dc_request("POST", "/api/v1/boards", kry_json_buf_finish(&b),
                        response, sizeof(response));
    kry_json_buf_free(&b);
    if(status != 201)
        return 0;
    krait_daochi_boards_refresh();
    for(int i = 0; i < g_dc.board_count; i++)
        if(strcmp(g_dc.boards[i].title, title) == 0) {
            g_dc.selected_board = i;
            break;
        }
    return 1;
}

int krait_daochi_board_select(int index)
{
    if(index < 0 || index >= g_dc.board_count)
        return 0;
    g_dc.selected_board = index;
    return 1;
}

int krait_daochi_board_selected(void)
{
    return g_dc.selected_board;
}

static DcBoard *
dc_selected_board(void)
{
    if(g_dc.selected_board < 0 || g_dc.selected_board >= g_dc.board_count)
        return NULL;
    return &g_dc.boards[g_dc.selected_board];
}

static int
dc_selected_path(char *dst, size_t dst_size, const char *suffix)
{
    const DcBoard *board = dc_selected_board();

    if(board == NULL)
        return 0;
    snprintf(dst, dst_size, "/api/v1/boards/%s%s", board->id, suffix);
    return 1;
}

int krait_daochi_board_detail_refresh(void)
{
    char path[192];
    char response[128 * 1024];
    long status;

    g_dc.member_count = 0;
    if(!dc_selected_path(path, sizeof(path), ""))
        return 0;
    status = dc_request("GET", path, NULL, response, sizeof(response));
    if(status != 200)
        return 0;
    {
        KryJson *root = kry_json_parse(response);
        const KryJson *list;

        if(root == NULL)
            return 0;
        list = kry_json_get(root, "members");
        for(int i = 0; i < kry_json_count(list) && g_dc.member_count < DC_MAX_MEMBERS; i++) {
            const KryJson *item = kry_json_at(list, i);
            DcMember *m = &g_dc.members[g_dc.member_count++];

            dc_json_string(item, "user_id_hash", m->id, sizeof(m->id));
            dc_json_string(item, "alias", m->alias, sizeof(m->alias));
            dc_json_string(item, "permission", m->permission, sizeof(m->permission));
        }
        kry_json_free(root);
    }
    return 1;
}

int krait_daochi_member_count(void) { return g_dc.member_count; }
const char *krait_daochi_member_id(int index)
{
    if(index < 0 || index >= g_dc.member_count) return "";
    return g_dc.members[index].id;
}
const char *krait_daochi_member_alias(int index)
{
    if(index < 0 || index >= g_dc.member_count) return "";
    return g_dc.members[index].alias[0] != '\0' ? g_dc.members[index].alias
                                                : g_dc.members[index].id;
}
const char *krait_daochi_member_permission(int index)
{
    if(index < 0 || index >= g_dc.member_count) return "";
    return g_dc.members[index].permission;
}

int krait_daochi_board_invite(const char *target, const char *permission)
{
    KryJsonBuf b = {0};
    char path[192];
    char response[8192];
    long status;

    if(!dc_selected_path(path, sizeof(path), "/members"))
        return 0;
    if(permission == NULL || permission[0] == '\0')
        permission = "read";
    kry_json_buf_raw(&b, "{\"target\":");
    kry_json_buf_str(&b, target);
    kry_json_buf_raw(&b, ",\"permission\":");
    kry_json_buf_str(&b, permission);
    kry_json_buf_raw(&b, "}");
    status = dc_request("POST", path, kry_json_buf_finish(&b), response, sizeof(response));
    kry_json_buf_free(&b);
    if(status == 403)
        dc_status("invite failed: must be friends and board owner");
    if(status == 409)
        dc_status("already a member");
    return status == 201;
}

int krait_daochi_member_set_permission(int member_index, const char *permission)
{
    char path[256];
    char response[4096];
    long status;

    if(member_index < 0 || member_index >= g_dc.member_count)
        return 0;
    if(!dc_selected_path(path, sizeof(path), "/members/"))
        return 0;
    strncat(path, g_dc.members[member_index].id, sizeof(path) - strlen(path) - 1);
    {
        KryJsonBuf b = {0};

        kry_json_buf_raw(&b, "{\"permission\":");
        kry_json_buf_str(&b, permission);
        kry_json_buf_raw(&b, "}");
        status = dc_request("PATCH", path, kry_json_buf_finish(&b), response, sizeof(response));
        kry_json_buf_free(&b);
    }
    return status == 200;
}

int krait_daochi_member_remove(int member_index)
{
    char path[256];
    char response[4096];
    long status;

    if(member_index < 0 || member_index >= g_dc.member_count)
        return 0;
    if(!dc_selected_path(path, sizeof(path), "/members/"))
        return 0;
    strncat(path, g_dc.members[member_index].id, sizeof(path) - strlen(path) - 1);
    status = dc_request("DELETE", path, NULL, response, sizeof(response));
    return status == 200;
}

int krait_daochi_board_archive(void)
{
    char path[192];
    char response[4096];
    long status;

    if(!dc_selected_path(path, sizeof(path), ""))
        return 0;
    status = dc_request("DELETE", path, NULL, response, sizeof(response));
    if(status == 200)
        krait_daochi_boards_refresh();
    return status == 200;
}

/* ------------------------------------------------------------------ */
/* board records                                                       */
/* ------------------------------------------------------------------ */

int krait_daochi_records_refresh(void)
{
    char path[192];
    char response[512 * 1024];
    long status;

    g_dc.record_count = 0;
    if(!dc_selected_path(path, sizeof(path), "/records?since=0"))
        return 0;
    status = dc_request("GET", path, NULL, response, sizeof(response));
    if(status != 200)
        return 0;
    {
        KryJson *root = kry_json_parse(response);
        const KryJson *list;

        if(root == NULL)
            return 0;
        list = kry_json_get(root, "records");
        for(int i = 0; i < kry_json_count(list) && g_dc.record_count < DC_MAX_RECORDS; i++) {
            const KryJson *item = kry_json_at(list, i);
            DcRecord *r = &g_dc.records[g_dc.record_count++];
            const KryJson *payload;

            dc_json_string(item, "id", r->id, sizeof(r->id));
            {
                const KryJson *n = kry_json_get(item, "updated_at");

                r->updated_at = n != NULL ? (long long)kry_json_number(n) : 0;
            }
            {
                const KryJson *d = kry_json_get(item, "deleted_at");

                r->deleted = d != NULL && kry_json_number(d) > 0;
            }
            payload = kry_json_get(item, "payload");
            /* compact payload: re-serialize the parsed tree */
            if(payload != NULL && kry_json_type(payload) == KRY_JSON_OBJECT) {
                KryJsonBuf b = {0};

                kry_json_buf_raw(&b, "{");
                for(int k = 0; k < kry_json_count(payload); k++) {
                    const char *key = kry_json_key(payload, k);
                    const KryJson *value = kry_json_at(payload, k);

                    if(k > 0)
                        kry_json_buf_raw(&b, ",");
                    kry_json_buf_str(&b, key);
                    kry_json_buf_raw(&b, ":");
                    switch(kry_json_type(value)) {
                    case KRY_JSON_STRING:
                        kry_json_buf_str(&b, kry_json_string(value));
                        break;
                    case KRY_JSON_NUMBER:
                        kry_json_buf_num(&b, kry_json_number(value));
                        break;
                    case KRY_JSON_BOOL:
                        kry_json_buf_raw(&b, kry_json_bool(value) ? "true" : "false");
                        break;
                    default:
                        kry_json_buf_raw(&b, "null");
                        break;
                    }
                }
                kry_json_buf_raw(&b, "}");
                snprintf(r->payload, sizeof(r->payload), "%s",
                         kry_json_buf_finish(&b));
                kry_json_buf_free(&b);
            } else {
                r->payload[0] = '\0';
            }
        }
        kry_json_free(root);
    }
    dc_status("%d record(s)", g_dc.record_count);
    return 1;
}

int krait_daochi_record_count(void) { return g_dc.record_count; }
const char *krait_daochi_record_id(int index)
{
    if(index < 0 || index >= g_dc.record_count) return "";
    return g_dc.records[index].id;
}
const char *krait_daochi_record_payload(int index)
{
    if(index < 0 || index >= g_dc.record_count) return "";
    return g_dc.records[index].payload;
}
int krait_daochi_record_deleted(int index)
{
    if(index < 0 || index >= g_dc.record_count) return 0;
    return g_dc.records[index].deleted;
}

static int
dc_record_put(const char *record_id, const char *payload_json)
{
    KryJsonBuf b = {0};
    char path[256];
    char response[4096];
    long status;

    if(record_id == NULL || record_id[0] == '\0' || payload_json == NULL)
        return 0;
    if(!dc_selected_path(path, sizeof(path), "/records/"))
        return 0;
    strncat(path, record_id, sizeof(path) - strlen(path) - 1);
    kry_json_buf_raw(&b, "{\"payload\":");
    kry_json_buf_raw(&b, payload_json);
    kry_json_buf_raw(&b, "}");
    status = dc_request("PUT", path, kry_json_buf_finish(&b), response, sizeof(response));
    kry_json_buf_free(&b);
    return status == 200;
}

/* payload field accessors used by the kanban share UI */
const char *krait_daochi_record_field(int index, const char *field)
{
    static char value[2048];
    KryJson *root;

    if(index < 0 || index >= g_dc.record_count || field == NULL)
        return "";
    value[0] = '\0';
    root = kry_json_parse(g_dc.records[index].payload);
    if(root != NULL) {
        const KryJson *v = kry_json_get(root, field);

        if(v != NULL && kry_json_type(v) == KRY_JSON_STRING)
            snprintf(value, sizeof(value), "%s", kry_json_string(v));
        kry_json_free(root);
    }
    return value;
}

int krait_daochi_record_put_fields(const char *record_id, const char *title,
                                   const char *body, const char *column)
{
    KryJsonBuf b = {0};
    int ok;

    if(record_id == NULL || record_id[0] == '\0')
        return 0;
    kry_json_buf_raw(&b, "{\"title\":");
    kry_json_buf_str(&b, title != NULL ? title : "");
    kry_json_buf_raw(&b, ",\"body\":");
    kry_json_buf_str(&b, body != NULL ? body : "");
    kry_json_buf_raw(&b, ",\"column\":");
    kry_json_buf_str(&b, column != NULL ? column : "backlog");
    kry_json_buf_raw(&b, "}");
    ok = dc_record_put(record_id, kry_json_buf_finish(&b));
    kry_json_buf_free(&b);
    return ok;
}

int krait_daochi_record_put_json(const char *record_id, const char *payload_json)
{
    return dc_record_put(record_id, payload_json);
}

int krait_daochi_record_delete(const char *record_id)
{
    char path[256];
    char response[4096];
    long status;

    if(record_id == NULL || record_id[0] == '\0')
        return 0;
    if(!dc_selected_path(path, sizeof(path), "/records/"))
        return 0;
    strncat(path, record_id, sizeof(path) - strlen(path) - 1);
    status = dc_request("DELETE", path, NULL, response, sizeof(response));
    return status == 200;
}

void krait_daochi_reset(void)
{
    int was_loaded = g_dc.account_loaded;

    memset(&g_dc, 0, sizeof(g_dc));
    g_dc.selected_board = -1;
    g_dc_node_url_cache[0] = '\0';
    g_dc.account_loaded = was_loaded;   /* keep the loaded flag semantics */
    if(was_loaded) {
        g_dc.account_loaded = 0;
        dc_load_account();              /* re-read from the current HOME */
    }
}
