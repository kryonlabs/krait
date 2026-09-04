/*
 * Game Engine mode unit tests. Drives the same krait_engine_* API the
 * editor UI uses: document edits, scene file round trip, play/stop with
 * physics and behaviors. Headless - no window, no drawing - mirroring the
 * kryon scene_tree tests (SceneTick/ScenePhysicsTick run without a GPU).
 */

#include "kryon.h"
#include "ide/state.h"
#include "native_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* User-defined behavior registered through the public registry (exactly
 * what a game plugin would do): drifts the node at vx/vy units per
 * second. */
static int g_drift_calls;

static void
drift_behavior(Scene *scene, NodeId node, float dt, const float *params,
               int param_count, void *user)
{
    Node *n = NodeGet(scene, node);

    (void)user;
    if(n == NULL || param_count < 2)
        return;
    g_drift_calls++;
    NodeSetPosition(scene, node,
                    n->local.position.x + params[0] * dt,
                    n->local.position.y + params[1] * dt);
}

int
main(void)
{
    int rc;

    {
        static const char *const drift_params[] = { "vx", "vy" };
        static const float drift_defaults[] = { 0.0f, 0.0f };

        if(krait_engine_behavior_register("drift", "Drift", drift_params,
                                          drift_defaults, 2, drift_behavior,
                                          NULL) < 0) {
            fprintf(stderr, "engine FAIL: behavior_register rejected drift\n");
            return 1;
        }
    }
    rc = krait_engine_smoke_test();


    if(rc != 0)
        return 1;

    /* a user-registered behavior runs during play and respects params */
    {
        int drifter;
        float dx0, dy0, dx1, dy1;
        int i;

        krait_engine_reset(NULL);
        krait_engine_advance(0.0f);
        drifter = krait_engine_add_node(0, "Drifter");
        if(drifter <= 0 || !krait_engine_set_behavior_id(drifter, "drift") ||
           !krait_engine_set_behavior_param(drifter, 0, 100.0f) ||
           !krait_engine_set_behavior_param(drifter, 1, -50.0f)) {
            fprintf(stderr, "engine FAIL: could not wire drift behavior\n");
            return 1;
        }
        if(!krait_engine_node_pos(drifter, &dx0, &dy0)) {
            fprintf(stderr, "engine FAIL: drifter missing\n");
            return 1;
        }
        g_drift_calls = 0;
        krait_engine_play();
        for(i = 0; i < 90; i++)
            krait_engine_advance(1.0f / 60.0f);
        if(!krait_engine_node_pos(drifter, &dx1, &dy1)) {
            fprintf(stderr, "engine FAIL: drifter missing while playing\n");
            return 1;
        }
        if(g_drift_calls < 60) {
            fprintf(stderr, "engine FAIL: drift behavior not applied (%d)\n",
                    g_drift_calls);
            return 1;
        }
        /* 1.5s at (100, -50)/s from the play snapshot position */
        if(fabsf(dx1 - dx0) < 120.0f || fabsf(dx1 - dx0) > 180.0f ||
           fabsf(dy1 - dy0) < 20.0f || fabsf(dy1 - dy0) > 80.0f) {
            fprintf(stderr, "engine FAIL: drift moved (%.1f, %.1f) -> "
                    "(%.1f, %.1f)\n", dx0, dy0, dx1, dy1);
            return 1;
        }
        /* behavior id + params survive a save/load round trip */
        {
            char tmp[512];
            float rx0, ry0, rx1, ry1;
            int i;

            snprintf(tmp, sizeof(tmp), "%s/krait-drift-roundtrip.scene",
                     getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
            remove(tmp);
            krait_engine_add_node(0, "Drifter");   /* re-add after stop */
            krait_engine_set_behavior_id(krait_engine_node_count(), "drift");
            if(!krait_engine_save(tmp)) {
                fprintf(stderr, "engine FAIL: drift round-trip save\n");
                return 1;
            }
            krait_engine_reset(NULL);
            if(!krait_engine_load(tmp)) {
                fprintf(stderr, "engine FAIL: drift round-trip load\n");
                return 1;
            }
            krait_engine_advance(0.0f);
            {
                int drifter2 = 0;
                int j;

                for(j = 1; j <= 24; j++) {
                    const char *nm = krait_engine_node_name(j);

                    if(nm != NULL && strcmp(nm, "Drifter") == 0)
                        drifter2 = j;
                }
                if(drifter2 == 0 ||
                   !krait_engine_set_behavior_param(drifter2, 0, 100.0f) ||
                   !krait_engine_set_behavior_param(drifter2, 1, -50.0f)) {
                    fprintf(stderr, "engine FAIL: drifter lost in round trip\n");
                    return 1;
                }
                if(!krait_engine_node_pos(drifter2, &rx0, &ry0)) {
                    fprintf(stderr, "engine FAIL: drifter2 missing\n");
                    return 1;
                }
                g_drift_calls = 0;
                krait_engine_play();
                for(i = 0; i < 30; i++)
                    krait_engine_advance(1.0f / 60.0f);
                if(!krait_engine_node_pos(drifter2, &rx1, &ry1) ||
                   fabsf(rx1 - rx0) < 30.0f || g_drift_calls < 20) {
                    fprintf(stderr, "engine FAIL: drift behavior did not "
                            "survive round trip (%.1f -> %.1f, %d calls)\n",
                            rx0, rx1, g_drift_calls);
                    return 1;
                }
                krait_engine_stop();
            }
            remove(tmp);
        }
    }

    /* 3D nodes: document model, headless build, serialization round trip */
    {
        char tmp[512];
        int cam3 = 0, cube = 0, sph = 0;
        float z = -9.0f, ry = -9.0f, sc = -9.0f;
        int i;

        snprintf(tmp, sizeof(tmp), "%s/krait-3d.scene",
                 getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
        remove(tmp);
        krait_engine_reset(NULL);
        if(krait_engine_scene_is_3d()) {
            fprintf(stderr, "engine FAIL: starter scene claims 3D\n");
            return 1;
        }
        krait_engine_advance(0.0f);
        cam3 = krait_engine_add_node(13, "Cam3");
        cube = krait_engine_add_node(12, "Cube");
        sph = krait_engine_add_node(12, "Ball");
        if(cam3 <= 0 || cube <= 0 || sph <= 0) {
            fprintf(stderr, "engine FAIL: could not add 3D nodes\n");
            return 1;
        }
        if(!krait_engine_scene_is_3d()) {
            fprintf(stderr, "engine FAIL: 3D scene not detected\n");
            return 1;
        }
        if(!krait_engine_set_3d(cube, 2.0f, 45.0f, 1.5f) ||
           !krait_engine_set_mesh(sph, 1 /* sphere */) ||
           !krait_engine_set_3d_target(cam3, 1.0f, 0.0f, -1.0f)) {
            fprintf(stderr, "engine FAIL: 3D setters rejected\n");
            return 1;
        }
        /* headless build of the 3D scene must not need a GPU */
        krait_engine_advance(0.0f);
        if(!krait_engine_save(tmp)) {
            fprintf(stderr, "engine FAIL: 3d save\n");
            return 1;
        }
        krait_engine_reset(NULL);
        if(!krait_engine_load(tmp)) {
            fprintf(stderr, "engine FAIL: 3d load\n");
            return 1;
        }
        if(!krait_engine_scene_is_3d()) {
            fprintf(stderr, "engine FAIL: 3D flag lost in round trip\n");
            return 1;
        }
        cube = 0;
        for(i = 1; i <= 24; i++) {
            const char *nm = krait_engine_node_name(i);

            if(nm != NULL && strcmp(nm, "Cube") == 0)
                cube = i;
            else if(nm != NULL && strcmp(nm, "Ball") == 0)
                sph = i;
        }
        if(!krait_engine_get_3d(cube, &z, &ry, &sc) ||
           fabsf(z - 2.0f) > 0.01f || fabsf(ry - 45.0f) > 0.01f ||
           fabsf(sc - 1.5f) > 0.01f) {
            fprintf(stderr, "engine FAIL: 3D transform lost (%.1f %.1f %.1f)\n",
                    z, ry, sc);
            return 1;
        }
        if(krait_engine_get_mesh(sph) != 1 /* sphere */) {
            fprintf(stderr, "engine FAIL: mesh kind lost in round trip\n");
            return 1;
        }
        krait_engine_stop();
        remove(tmp);
    }

    /* kscript: scene-authored game logic runs during play, round trips,
     * and drives triggers without any recompile */
    {
        char tmp[512];
        int ball, flag;
        int i;

        snprintf(tmp, sizeof(tmp), "%s/krait-script.scene",
                 getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
        remove(tmp);
        krait_engine_reset(NULL);
        krait_engine_advance(0.0f);
        ball = krait_engine_add_node(0, "Ball");
        flag = krait_engine_add_node(0, "Flag");
        if(ball <= 0 || flag <= 0 ||
           !krait_engine_node_set_script(ball,
               "set vy = vy - 100 * dt\n"
               "set y = y + vy * dt\n"
               "if y < 50 then set vy = 0\n"
               "set y = 50") ||
           !krait_engine_node_set_script(flag,
               "if time > 0.5 then set win = 1")) {
            fprintf(stderr, "engine FAIL: script setup rejected\n");
            return 1;
        }
        /* script survives the save/load round trip */
        if(!krait_engine_save(tmp)) {
            fprintf(stderr, "engine FAIL: script save\n");
            return 1;
        }
        krait_engine_reset(NULL);
        if(!krait_engine_load(tmp)) {
            fprintf(stderr, "engine FAIL: script load\n");
            return 1;
        }
        krait_engine_advance(0.0f);
        ball = flag = 0;
        for(i = 1; i <= 16; i++) {
            const char *nm = krait_engine_node_name(i);

            if(nm != NULL && strcmp(nm, "Ball") == 0)
                ball = i;
            else if(nm != NULL && strcmp(nm, "Flag") == 0)
                flag = i;
        }
        krait_engine_play();
        for(i = 0; i < 90; i++)
            krait_engine_advance(1.0f / 60.0f);
        {
            float bx = 0.0f, by = 0.0f;

            /* the ball script clamps y to 50 via the conditional */
            if(!krait_engine_node_pos(ball, &bx, &by) ||
               fabsf(by - 50.0f) > 0.01f) {
                fprintf(stderr, "engine FAIL: script did not run (%.1f)\n",
                        by);
                return 1;
            }
        }
        /* the flag script fired the win trigger after 0.5s */
        if(!krait_engine_won()) {
            fprintf(stderr, "engine FAIL: script win trigger not fired\n");
            return 1;
        }
        krait_engine_stop();
        remove(tmp);
    }

    /* 3D behaviors: spin3d rotation + player3d gravity/ground (headless) */
    {
        int spinner3, walker;
        float z = 0.0f, ry = 0.0f, sc = 0.0f;
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        int i;

        krait_engine_reset(NULL);
        krait_engine_advance(0.0f);
        spinner3 = krait_engine_add_node(12, "Spinner3");
        walker = krait_engine_add_node(12, "Walker");
        if(spinner3 <= 0 || walker <= 0 ||
           !krait_engine_set_behavior_id(spinner3, "spin3d") ||
           !krait_engine_set_behavior_id(walker, "player3d") ||
           !krait_engine_node_set_pos3(walker, 0.0f, 2.0f, 0.0f)) {
            fprintf(stderr, "engine FAIL: 3d behavior setup rejected\n");
            return 1;
        }
        krait_engine_play();
        for(i = 0; i < 90; i++)
            krait_engine_advance(1.0f / 60.0f);
        if(!krait_engine_get_3d(spinner3, &z, &ry, &sc) || ry < 1.0f) {
            fprintf(stderr, "engine FAIL: spin3d did not rotate (%.2f)\n",
                    ry);
            return 1;
        }
        /* no keys held: gravity drops the walker from y=2 onto ground 0
         * (fall time ~0.64s, then the clamp holds it at 0) */
        if(!krait_engine_node_pos3(walker, &wx, &wy, &wz) ||
           fabsf(wy) > 0.001f) {
            fprintf(stderr, "engine FAIL: player3d did not land (%.2f)\n",
                    wy);
            return 1;
        }
        krait_engine_stop();
        /* stop restores the authored height (2.0) */
        if(!krait_engine_node_pos3(walker, &wx, &wy, &wz) ||
           fabsf(wy - 2.0f) > 0.001f) {
            fprintf(stderr, "engine FAIL: 3d behavior mutated document "
                    "after stop (%.2f)\n", wy);
            return 1;
        }
    }

    /* timeline editing: key ops + scrub preview on an AnimationPlayer */
    {
        int target, anim;
        float t0 = -1.0f, v0 = -1.0f;
        int i;

        krait_engine_reset(NULL);
        krait_engine_advance(0.0f);
        target = krait_engine_add_node(0, "Mover2");
        anim = krait_engine_add_node(8, "Tween2");
        if(target <= 0 || anim <= 0) {
            fprintf(stderr, "engine FAIL: timeline nodes not created\n");
            return 1;
        }
        /* default track targets the selected node (Mover2) on X with
         * keys {0, x0}, {1, x0+200} */
        if(krait_engine_anim_key_count(anim, 0) != 2) {
            fprintf(stderr, "engine FAIL: default track keys %d\n",
                    krait_engine_anim_key_count(anim, 0));
            return 1;
        }
        if(!krait_engine_anim_add_key(anim, 0, 0.5f, 999.0f) ||
           krait_engine_anim_key_count(anim, 0) != 3) {
            fprintf(stderr, "engine FAIL: add key rejected\n");
            return 1;
        }
        /* the inserted key sorts between the two others */
        if(!krait_engine_anim_key_get(anim, 0, 1, &t0, &v0) ||
           fabsf(t0 - 0.5f) > 0.001f || fabsf(v0 - 999.0f) > 0.001f) {
            fprintf(stderr, "engine FAIL: added key not sorted in\n");
            return 1;
        }
        if(!krait_engine_anim_move_key(anim, 0, 1, 0.75f) ||
           !krait_engine_anim_key_get(anim, 0, 1, &t0, &v0) ||
           fabsf(t0 - 0.75f) > 0.001f) {
            fprintf(stderr, "engine FAIL: move key rejected\n");
            return 1;
        }
        if(!krait_engine_anim_delete_key(anim, 0, 1) ||
           krait_engine_anim_key_count(anim, 0) != 2) {
            fprintf(stderr, "engine FAIL: delete key rejected\n");
            return 1;
        }
        /* scrub preview applies runtime-only values: with keys {0,x0}
         * and {1,x0+200}, scrubbing to 0.5 puts the target halfway */
        {
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

            if(!krait_engine_node_pos(target, &x0, &y0) ||
               !krait_engine_timeline_scrub(anim, 0.5f) ||
               !krait_engine_node_pos(target, &x1, &y1)) {
                fprintf(stderr, "engine FAIL: scrub failed\n");
                return 1;
            }
            if(fabsf(x1 - (x0 + 100.0f)) > 0.5f) {
                fprintf(stderr, "engine FAIL: scrub preview at %.1f "
                        "(want ~%.1f)\n", x1, x0 + 100.0f);
                return 1;
            }
            /* the authored document is untouched by scrubbing */
            krait_engine_stop();
            if(!krait_engine_node_pos(target, &x1, &y1) ||
               fabsf(x1 - x0) > 0.001f) {
                fprintf(stderr, "engine FAIL: scrub mutated the document\n");
                return 1;
            }
        }
        (void)i;
    }

    /* Particles2D: configured rate/lifetime drive the steady state, the
     * emitter config round trips, and Stop clears the runtime */
    {
        char tmp[512];
        int emitter = 0;
        int i;

        snprintf(tmp, sizeof(tmp), "%s/krait-particles.scene",
                 getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
        remove(tmp);
        krait_engine_reset(NULL);
        krait_engine_advance(0.0f);
        for(i = 1; i <= 16; i++) {
            const char *nm = krait_engine_node_name(i);

            if(nm != NULL && strcmp(nm, "Sparkles") == 0)
                emitter = i;
        }
        if(emitter == 0 ||
           !krait_engine_set_particles(emitter, 60.0f, 1.0f, 100.0f,
                                       360.0f)) {
            fprintf(stderr, "engine FAIL: no emitter to configure\n");
            return 1;
        }
        if(!krait_engine_save(tmp)) {
            fprintf(stderr, "engine FAIL: particles save\n");
            return 1;
        }
        krait_engine_reset(NULL);
        if(!krait_engine_load(tmp)) {
            fprintf(stderr, "engine FAIL: particles load\n");
            return 1;
        }
        krait_engine_advance(0.0f);
        emitter = 0;
        for(i = 1; i <= 16; i++) {
            const char *nm = krait_engine_node_name(i);

            if(nm != NULL && strcmp(nm, "Sparkles") == 0)
                emitter = i;
        }
        krait_engine_play();
        for(i = 0; i < 90; i++)
            krait_engine_advance(1.0f / 60.0f);
        {
            int alive = krait_engine_particle_count(emitter);

            /* 1.5s at 60/s with 1s lifetime: ~60 alive (cap 256) */
            if(alive < 45 || alive > 75) {
                fprintf(stderr,
                        "engine FAIL: particle count after round trip %d\n",
                        alive);
                return 1;
            }
        }
        krait_engine_stop();
        if(krait_engine_particle_count(emitter) != 0) {
            fprintf(stderr, "engine FAIL: particles survived Stop\n");
            return 1;
        }
        remove(tmp);
    }

    /* pause/resume keeps the simulation frozen */
    krait_engine_reset(NULL);
    krait_engine_advance(0.0f);
    krait_engine_play();
    krait_engine_advance(1.0f / 60.0f);
    krait_engine_pause();
    {
        int i;

        for(i = 0; i < 30; i++)
            krait_engine_advance(1.0f / 60.0f);
    }
    if(!krait_engine_paused()) {
        fprintf(stderr, "engine FAIL: pause did not stick\n");
        return 1;
    }
    krait_engine_stop();
    if(krait_engine_playing()) {
        fprintf(stderr, "engine FAIL: stop did not stop\n");
        return 1;
    }

    /* scaffolded projects carry a playable game.scene starter */
    {
        char dir[256];
        char scene[320];
        char status[256];
        int i;
        int player = 0, coins = 0, exit_area = 0, ticker = 0, float_anim = 0;
        int coin2 = 0;
        float coin2_y0 = 0.0f, coin2_y1 = 0.0f, coin2_x = 0.0f;

        snprintf(dir, sizeof(dir), "%s/krait-engine-scaffold",
                 getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
        snprintf(scene, sizeof(scene), "%s/game.scene", dir);
        remove(scene);
        if(!krait_scaffold_project(dir, status, sizeof(status))) {
            fprintf(stderr, "engine FAIL: scaffold: %s\n", status);
            return 1;
        }
        if(access(scene, R_OK) != 0) {
            fprintf(stderr, "engine FAIL: scaffold wrote no game.scene\n");
            return 1;
        }
        krait_engine_reset(dir);
        if(!krait_engine_load(scene)) {
            fprintf(stderr, "engine FAIL: cannot load scaffolded scene\n");
            return 1;
        }
        krait_engine_advance(0.0f);
        if(krait_engine_node_count() != 9) {
            fprintf(stderr, "engine FAIL: scaffold scene node count %d\n",
                    krait_engine_node_count());
            return 1;
        }
        for(i = 1; i <= 12; i++) {
            const char *nm = krait_engine_node_name(i);

            if(nm == NULL)
                continue;
            if(strcmp(nm, "Player") == 0)
                player = i;
            else if(strcmp(nm, "Coin2") == 0) {
                coins++;
                coin2 = i;
            } else if(strcmp(nm, "Coin") == 0)
                coins++;
            else if(strcmp(nm, "Exit") == 0)
                exit_area = i;
            else if(strcmp(nm, "ScoreTicker") == 0)
                ticker = i;
            else if(strcmp(nm, "FloatAnim") == 0)
                float_anim = i;
        }
        if(player == 0 || coins != 2 || exit_area == 0 || ticker == 0 ||
           float_anim == 0 || coin2 == 0) {
            fprintf(stderr,
                    "engine FAIL: scaffold starter incomplete "
                    "(player %d coins %d exit %d ticker %d anim %d)\n",
                    player, coins, exit_area, ticker, float_anim);
            return 1;
        }
        krait_engine_node_pos(coin2, &coin2_x, &coin2_y0);
        /* the player plays: 1.5s of simulation with the physics player */
        krait_engine_play();
        for(i = 0; i < 90; i++)
            krait_engine_advance(1.0f / 60.0f);
        if(krait_engine_score() < 1) {
            fprintf(stderr,
                    "engine FAIL: scaffold score ticker did not fire (%d)\n",
                    krait_engine_score());
            return 1;
        }
        /* FloatAnim bobs Coin2 (loop 1.5s: 120 -> 90 -> 120) */
        if(!krait_engine_node_pos(coin2, &coin2_x, &coin2_y1) ||
           fabsf(coin2_y1 - coin2_y0) < 2.0f) {
            fprintf(stderr, "engine FAIL: Coin2 not animated (%.1f -> %.1f)\n",
                    coin2_y0, coin2_y1);
            return 1;
        }
        krait_engine_stop();

        /* export writes a standalone launcher next to the scene */
        {
            char runsh[400];

            if(!krait_engine_export_game(status, sizeof(status))) {
                fprintf(stderr, "engine FAIL: export: %s\n", status);
                return 1;
            }
            snprintf(runsh, sizeof(runsh),
                     "%s/game-export/run.sh", dir);
            if(access(runsh, X_OK) != 0) {
                fprintf(stderr, "engine FAIL: export wrote no runnable %s\n",
                        runsh);
                return 1;
            }
            snprintf(runsh, sizeof(runsh), "%s/game-export/game.scene", dir);
            if(access(runsh, R_OK) != 0) {
                fprintf(stderr, "engine FAIL: export copied no scene\n");
                return 1;
            }
        }
        remove(scene);
    }

    fprintf(stderr, "engine test ok\n");
    return 0;
}
