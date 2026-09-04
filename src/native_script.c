/*
 * kscript: the Game Engine's scene-authored scripting layer.
 *
 * A tiny line-based data language stored inside game.scene so games are
 * authorable end-to-end without recompiling krait. Scripts run once per
 * frame while playing, after behaviors, on the runtime pose (the authored
 * document is never mutated). Statements are separated by ';' or
 * newlines:
 *
 *   set vy = vy - 9.8 * dt
 *   set y = y + vy * dt
 *   if y < 0.5 then set vy = 5
 *   if keyp("space") then collect("Coin")
 *   set x = x + sin(time * 2) * dt
 *
 * Expressions: numbers, per-node variables, node properties (x, y, z,
 * roty, score), builtins dt/time/key/keyp/sin/cos/abs/min/max/rand/
 * nx/ny, + - * /, comparisons, and/or/not, parentheses.
 */

#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"
#include "native_engine_internal.h"
#include "native_script.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KT_END 0
#define KT_NUM 1
#define KT_NAME 2
#define KT_STR 3
#define KT_OP 4

typedef struct {
    EngineNode *node;
    float dt;
    const char *p;
    char tok[64];
    char last_str[64];
    int tok_kind;
    float tok_num;
} KscriptCtx;

static void
ks_next(KscriptCtx *c)
{
    while(*c->p == ' ' || *c->p == '\t' || *c->p == '\r')
        c->p++;
    if(*c->p == '\0' || *c->p == '\n' || *c->p == ';') {
        /* statement separator: normalise to ';' */
        c->tok[0] = *c->p == '\0' ? '\0' : ';';
        if(*c->p == '\0')
            c->tok[0] = '\0';
        c->tok[1] = '\0';
        c->tok_kind = *c->p == '\0' ? KT_END : KT_OP;
        if(*c->p != '\0')
            c->p++;
        return;
    }
    if(isdigit((unsigned char)*c->p) ||
       (*c->p == '.' && isdigit((unsigned char)c->p[1]))) {
        char *end;

        c->tok_num = strtof(c->p, &end);
        snprintf(c->tok, sizeof(c->tok), "%.*s", (int)(end - c->p), c->p);
        c->p = end;
        c->tok_kind = KT_NUM;
        return;
    }
    if(isalpha((unsigned char)*c->p) || *c->p == '_') {
        int n = 0;

        while((isalnum((unsigned char)*c->p) || *c->p == '_') &&
              n < (int)sizeof(c->tok) - 1)
            c->tok[n++] = *c->p++;
        c->tok[n] = '\0';
        c->tok_kind = KT_NAME;
        return;
    }
    if(*c->p == '"') {
        int n = 0;

        c->p++;
        while(*c->p != '"' && *c->p != '\0' && *c->p != '\n' &&
              n < (int)sizeof(c->tok) - 1)
            c->tok[n++] = *c->p++;
        if(*c->p == '"')
            c->p++;
        c->tok[n] = '\0';
        snprintf(c->last_str, sizeof(c->last_str), "%s", c->tok);
        c->tok_kind = KT_STR;
        return;
    }
    if(strncmp(c->p, "<=", 2) == 0 || strncmp(c->p, ">=", 2) == 0 ||
       strncmp(c->p, "==", 2) == 0 || strncmp(c->p, "!=", 2) == 0 ||
       strncmp(c->p, "&&", 2) == 0 || strncmp(c->p, "||", 2) == 0) {
        c->tok[0] = *c->p++;
        c->tok[1] = *c->p++;
        c->tok[2] = '\0';
        c->tok_kind = KT_OP;
        return;
    }
    c->tok[0] = *c->p++;
    c->tok[1] = '\0';
    c->tok_kind = KT_OP;
}

static int
ks_is_op(KscriptCtx *c, const char *op)
{
    return c->tok_kind == KT_OP && strcmp(c->tok, op) == 0;
}

static void
ks_end_statement(KscriptCtx *c)
{
    while(c->tok_kind != KT_END && !ks_is_op(c, ";"))
        ks_next(c);
    if(ks_is_op(c, ";"))
        ks_next(c);
}

/* engine hooks (native_engine.c) */
float krait_script_time(void);
float krait_script_score(void);
void krait_script_add_score(float v);
void krait_script_win(void);
void krait_script_collect(const char *name);
float krait_script_node_x(const char *name);
float krait_script_node_y(const char *name);
int krait_script_key_down(const char *name);
int krait_script_key_pressed(const char *name);
unsigned krait_script_rand(EngineNode *node);
Scene *krait_script_scene_ptr(void);

static float
ks_call(KscriptCtx *c, const char *name, int argc, const float *argv)
{
    if(strcmp(name, "sin") == 0 && argc >= 1)
        return sinf(argv[0]);
    if(strcmp(name, "cos") == 0 && argc >= 1)
        return cosf(argv[0]);
    if(strcmp(name, "abs") == 0 && argc >= 1)
        return fabsf(argv[0]);
    if(strcmp(name, "min") == 0 && argc >= 2)
        return argv[0] < argv[1] ? argv[0] : argv[1];
    if(strcmp(name, "max") == 0 && argc >= 2)
        return argv[0] > argv[1] ? argv[0] : argv[1];
    if(strcmp(name, "rand") == 0 && argc >= 2)
        return argv[0] + (float)(krait_script_rand(c->node) % 10000u) /
                           9999.0f * (argv[1] - argv[0]);
    if(strcmp(name, "key") == 0)
        return krait_script_key_down(c->last_str) ? 1.0f : 0.0f;
    if(strcmp(name, "keyp") == 0)
        return krait_script_key_pressed(c->last_str) ? 1.0f : 0.0f;
    if(strcmp(name, "nx") == 0)
        return krait_script_node_x(c->last_str);
    if(strcmp(name, "ny") == 0)
        return krait_script_node_y(c->last_str);
    (void)argc;
    (void)argv;
    return 0.0f;
}

static float ks_expr(KscriptCtx *c);

static float
ks_pos_x(KscriptCtx *c)
{
    Node *rn = c->node->runtime >= 0
        ? NodeGet(krait_script_scene_ptr(), c->node->runtime) : NULL;

    if(rn != NULL)
        return rn->local.position.x;
    return c->node->r_x;
}

static float
ks_pos_y(KscriptCtx *c)
{
    Node *rn = c->node->runtime >= 0
        ? NodeGet(krait_script_scene_ptr(), c->node->runtime) : NULL;

    if(rn != NULL)
        return rn->local.position.y;
    return c->node->r_y;
}

static void
ks_set_pos(KscriptCtx *c, int is_y, float v)
{
    Node *rn = c->node->runtime >= 0
        ? NodeGet(krait_script_scene_ptr(), c->node->runtime) : NULL;

    if(rn != NULL) {
        if(is_y)
            rn->local.position.y = v;
        else
            rn->local.position.x = v;
        rn->flags |= NODE_FLAG_DIRTY;
    }
    if(is_y)
        c->node->r_y = v;
    else
        c->node->r_x = v;
}

static float
ks_primary(KscriptCtx *c)
{
    float v = 0.0f;

    if(c->tok_kind == KT_NUM) {
        v = c->tok_num;
        ks_next(c);
        return v;
    }
    if(c->tok_kind == KT_STR) {
        ks_next(c);
        return 0.0f;   /* strings feed key()/nx()... via last_str */
    }
    if(c->tok_kind == KT_OP && strcmp(c->tok, "-") == 0) {
        ks_next(c);
        return -ks_primary(c);
    }
    if(c->tok_kind == KT_OP && strcmp(c->tok, "!") == 0) {
        ks_next(c);
        return ks_primary(c) == 0.0f ? 1.0f : 0.0f;
    }
    if(c->tok_kind == KT_OP && strcmp(c->tok, "(") == 0) {
        ks_next(c);
        v = ks_expr(c);
        if(ks_is_op(c, ")"))
            ks_next(c);
        return v;
    }
    if(c->tok_kind == KT_NAME) {
        char name[64];

        snprintf(name, sizeof(name), "%s", c->tok);
        ks_next(c);
        if(ks_is_op(c, "(")) {
            float args[4];
            int argc = 0;

            ks_next(c);
            while(c->tok_kind != KT_END && !ks_is_op(c, ")") &&
                  !ks_is_op(c, ";") && argc < 4) {
                args[argc++] = ks_expr(c);
                if(ks_is_op(c, ","))
                    ks_next(c);
                else
                    break;
            }
            if(ks_is_op(c, ")"))
                ks_next(c);
            return ks_call(c, name, argc, args);
        }
        if(strcmp(name, "dt") == 0)
            return c->dt;
        if(strcmp(name, "time") == 0)
            return krait_script_time();
        if(strcmp(name, "score") == 0)
            return krait_script_score();
        if(strcmp(name, "x") == 0)
            return ks_pos_x(c);
        if(strcmp(name, "y") == 0)
            return ks_pos_y(c);
        if(strcmp(name, "z") == 0)
            return c->node->r_z;
        if(strcmp(name, "roty") == 0)
            return c->node->r_rot_y;
        /* per-node persistent variables live on the node */
        {
            int i;

            for(i = 0; i < c->node->script_var_count; i++) {
                if(strcmp(c->node->script_var_names[i], name) == 0)
                    return c->node->script_vars[i];
            }
        }
        return 0.0f;
    }
    ks_next(c);
    return 0.0f;
}

static float
ks_term(KscriptCtx *c)
{
    float v = ks_primary(c);

    for(;;) {
        if(ks_is_op(c, "*")) {
            ks_next(c);
            v = v * ks_primary(c);
        } else if(ks_is_op(c, "/")) {
            float d;

            ks_next(c);
            d = ks_primary(c);
            v = d != 0.0f ? v / d : 0.0f;
        } else
            break;
    }
    return v;
}

static float
ks_addsub(KscriptCtx *c)
{
    float v = ks_term(c);

    for(;;) {
        if(ks_is_op(c, "+")) {
            ks_next(c);
            v = v + ks_term(c);
        } else if(ks_is_op(c, "-")) {
            ks_next(c);
            v = v - ks_term(c);
        } else
            break;
    }
    return v;
}

static float
ks_expr(KscriptCtx *c)
{
    float v = ks_addsub(c);

    if(ks_is_op(c, "<")) {
        ks_next(c);
        return v < ks_addsub(c) ? 1.0f : 0.0f;
    }
    if(ks_is_op(c, ">")) {
        ks_next(c);
        return v > ks_addsub(c) ? 1.0f : 0.0f;
    }
    if(ks_is_op(c, "<=")) {
        ks_next(c);
        return v <= ks_addsub(c) ? 1.0f : 0.0f;
    }
    if(ks_is_op(c, ">=")) {
        ks_next(c);
        return v >= ks_addsub(c) ? 1.0f : 0.0f;
    }
    if(ks_is_op(c, "==")) {
        ks_next(c);
        return v == ks_addsub(c) ? 1.0f : 0.0f;
    }
    if(ks_is_op(c, "!=")) {
        ks_next(c);
        return v != ks_addsub(c) ? 1.0f : 0.0f;
    }
    for(;;) {
        if(ks_is_op(c, "&&")) {
            ks_next(c);
            v = (v != 0.0f && ks_addsub(c) != 0.0f) ? 1.0f : 0.0f;
        } else if(ks_is_op(c, "||")) {
            ks_next(c);
            v = (v != 0.0f || ks_addsub(c) != 0.0f) ? 1.0f : 0.0f;
        } else
            break;
    }
    return v;
}

static void
ks_assign(KscriptCtx *c, const char *name, float v)
{
    if(strcmp(name, "x") == 0) {
        ks_set_pos(c, 0, v);
        return;
    }
    if(strcmp(name, "y") == 0) {
        ks_set_pos(c, 1, v);
        return;
    }
    if(strcmp(name, "z") == 0) {
        c->node->r_z = v;
        return;
    }
    if(strcmp(name, "roty") == 0) {
        c->node->r_rot_y = v;
        return;
    }
    if(strcmp(name, "score") == 0) {
        krait_script_add_score(v);
        return;
    }
    if(strcmp(name, "win") == 0) {
        if(v != 0.0f)
            krait_script_win();
        return;
    }
    {
        int i;

        for(i = 0; i < c->node->script_var_count; i++) {
            if(strcmp(c->node->script_var_names[i], name) == 0) {
                c->node->script_vars[i] = v;
                return;
            }
        }
        if(c->node->script_var_count < ENGINE_SCRIPT_VARS_MAX) {
            snprintf(c->node->script_var_names[c->node->script_var_count],
                     ENGINE_SCRIPT_NAME_MAX, "%s", name);
            c->node->script_vars[c->node->script_var_count] = v;
            c->node->script_var_count++;
        }
    }
}

static void
ks_statement(KscriptCtx *c)
{
    if(c->tok_kind == KT_NAME && strcmp(c->tok, "set") == 0) {
        char name[64];

        ks_next(c);
        if(c->tok_kind != KT_NAME) {
            ks_end_statement(c);
            return;
        }
        snprintf(name, sizeof(name), "%s", c->tok);
        ks_next(c);
        if(!ks_is_op(c, "=")) {
            ks_end_statement(c);
            return;
        }
        ks_next(c);
        ks_assign(c, name, ks_expr(c));
        ks_end_statement(c);
        return;
    }
    if(c->tok_kind == KT_NAME && strcmp(c->tok, "if") == 0) {
        float cond;

        ks_next(c);
        cond = ks_expr(c);
        if(c->tok_kind == KT_NAME && strcmp(c->tok, "then") == 0)
            ks_next(c);
        if(cond != 0.0f)
            ks_statement(c);
        else
            ks_end_statement(c);
        return;
    }
    if(c->tok_kind == KT_NAME && strcmp(c->tok, "collect") == 0) {
        ks_next(c);
        if(ks_is_op(c, "(")) {
            ks_next(c);
            if(c->tok_kind == KT_STR) {
                krait_script_collect(c->tok);
                ks_next(c);
            }
            if(ks_is_op(c, ")"))
                ks_next(c);
        }
        ks_end_statement(c);
        return;
    }
    if(c->tok_kind == KT_NAME && strcmp(c->tok, "win") == 0) {
        ks_next(c);
        krait_script_win();
        ks_end_statement(c);
        return;
    }
    if(c->tok_kind != KT_END)
        ks_expr(c);
    ks_end_statement(c);
}

void
krait_script_run(EngineNode *node, float dt)
{
    KscriptCtx c;

    if(node == NULL || node->script[0] == '\0')
        return;
    memset(&c, 0, sizeof(c));
    c.node = node;
    c.dt = dt;
    c.p = node->script;
    ks_next(&c);
    while(c.tok_kind != KT_END) {
        if(ks_is_op(&c, ";")) {
            ks_next(&c);
            continue;
        }
        ks_statement(&c);
    }
}
