/* Host harness for the battlefield renderer: plays AI-vs-AI shots with a chosen tank/ammo through the real battle core
 * and the DS field renderer (render.c / particles.c / gfx.c compiled for the PC) and checks every frame for sanity:
 * finite flight/explosion/particle values and no "stretched" frames (whole rows of one colour across the field).
 *   hostfield <design> <ammo> <map> <seeds> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/battle.h"
#include "../../core/session.h"
#include "../../source/render.h"

uint32_t plat_lines(void) { return 0; }
void plat_log(const char *fmt, ...) { (void)fmt; }

static Px topbuf[256 * 192];

static int stretched_rows(const Surf *s) {
    int rows = 0;
    for (int y = 16; y < 16 + 160; y += 4) {
        const Px *r = s->px + y * s->stride;
        bool same = true;
        for (int x = 8; x < 256 && same; x += 8) same = r[x] == r[0];
        rows += same;
    }
    return rows;
}

int main(int argc, char **argv) {
    int design = atoi(argv[1]), ammo = atoi(argv[2]), map = atoi(argv[3]), seeds = atoi(argv[4]);
    static SaveData save;
    static Session sess;
    static Battle b;
    static FieldView fv;
    Settings st;
    save_new(&save);
    st = save.settings;
    st.effects = 1;
    int bad = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        PlayerSetup p[2];
        player_setup_init(&p[0], "P0", "P0", (RGB){80, 200, 255}, (RGB){255, 255, 255}, false, design, ammo, 3);
        player_setup_init(&p[1], "P1", "P1", (RGB){255, 120, 80}, (RGB){255, 255, 255}, true, design, ammo, 3);
        p[0].team = 0; p[1].team = 1;
        session_init_match(&sess, p, 2, MODE_SINGLE, DIFF_HARD, 2, "TEST", true, false, false, false, map, (uint32_t)seed);
        battle_init(&b, &sess, &save, &st);
        field_init(&fv, &b, &st);
        Surf top = {topbuf, 256, 192, 256};
        double t = 0;
        for (int f = 0; f < 30 * 60; f++) {
            if (b.phase == PH_AIMING && battle_current(&b)->index == 0) {      /* the human: 50 degrees, 60% (+ a little per seed) */
                Tank *me = battle_current(&b);
                me->angle = 50 + (seed % 7); me->power = 60 + (seed % 5);
                battle_fire(&b);
            }
            battle_update(&b, 1 / 30.0);
            field_update(&fv, &b, 1 / 30.0f);
            t += 1 / 30.0;
            field_draw(&fv, &b, (float)t);
            field_blit(&fv, &b, &top, 0, 16);
            int sr = stretched_rows(&top);
            bool nonfinite = !isfinite(b.shake);
            for (int i = 0; i < b.nflights; i++) if (!isfinite(b.flights[i].x) || !isfinite(b.flights[i].y)) nonfinite = true;
            for (int i = 0; i < b.nexplosions; i++) if (!isfinite(b.explosions[i].x) || !isfinite(b.explosions[i].radius)) nonfinite = true;
            if (sr >= 10 || nonfinite) {
                printf("seed %d frame %d: stretched rows %d, shake %g, phase %d, flights %d, explosions %d\n", seed, f, sr, b.shake, b.phase, b.nflights, b.nexplosions);
                for (int i = 0; i < b.nexplosions; i++) printf("  expl %g %g r %g age %g dur %g style %d\n", b.explosions[i].x, b.explosions[i].y, b.explosions[i].radius, b.explosions[i].age, b.explosions[i].duration, b.explosions[i].style);
                for (int i = 0; i < b.nflights; i++) printf("  flight %g %g t %g\n", b.flights[i].x, b.flights[i].y, b.flights[i].t);
                bad++;
                break;
            }
            if (b.round_over) break;
        }
    }
    printf("%d of %d matches had a bad frame\n", bad, seeds);
    return bad != 0;
}
