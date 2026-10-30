/* Host-side probe: runs the native core on given inputs and prints results in a fixed text format that
 * tests/parity/run_parity.py compares against the Python reference. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/world.h"
#include "../../core/shot.h"
#include "../../core/ai.h"
#include "../../core/battle.h"

/* int64 to decimal without %lld (long is 32-bit on the DS and its printf may lack long long) */
static const char *i64str(int64_t v, char *buf) {
    char tmp[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    int k = 0;
    if (v < 0) buf[k++] = '-';
    while (n) buf[k++] = tmp[--n];
    buf[k] = 0;
    return buf;
}

static void mkplayer(PlayerSetup *p, int i, int design, int ammo, int team, double scale, double hover) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof p->name, "P%d", i);
    strcpy(p->label, "X");
    p->tank_color = COLOR_CHOICES[i].c;
    p->shot_color = COLOR_CHOICES[i + 4].c;
    p->design = design; p->ammo = ammo; p->hp = 3; p->team = team; p->diff = -1; p->prec = 1.0;
    p->scale = scale; p->hover = hover; p->kind = scale != 1.0 ? KIND_UFO : KIND_TANK;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (!strcmp(argv[1], "world")) {
        /* world <map> <seed> <wind> <n> [design ammo team scale hover]... */
        int map = atoi(argv[2]); unsigned seed = (unsigned)strtoul(argv[3], 0, 10); double wind = atof(argv[4]);
        int n = atoi(argv[5]);
        PlayerSetup pl[MAX_TANKS];
        for (int i = 0; i < n; i++)
            mkplayer(&pl[i], i, atoi(argv[6 + i * 5]), atoi(argv[7 + i * 5]), atoi(argv[8 + i * 5]), atof(argv[9 + i * 5]), atof(argv[10 + i * 5]));
        static World w;
        world_init(&w, FIELD_W, FIELD_H, map, seed, pl, n, wind);
        printf("H");
        for (int x = 0; x < w.width; x++) printf(" %d", w.terrain.heights[x]);
        printf("\nW %.17g\n", w.wind);
        for (int i = 0; i < n; i++) printf("T %.17g %.17g %d %.17g\n", w.tanks[i].x, w.tanks[i].y, w.tanks[i].facing, w.tanks[i].angle);
        printf("S %d %d %.17g\n", w.nstars, w.nmotes, w.nstreaks ? w.streaks[w.nstreaks - 1][1] : 0.0);
        /* then shots: argv after players: shooter angle power ammo_fx special */
        int k = 6 + n * 5;
        while (k + 4 < argc + 0 && k + 4 <= argc - 1) {
            int sh = atoi(argv[k]); double ang = atof(argv[k + 1]), pow_ = atof(argv[k + 2]);
            int fx = atoi(argv[k + 3]), sp = atoi(argv[k + 4]);
            k += 5;
            w.ammo_fx = fx != 0;
            w.tanks[sh].angle = ang; w.tanks[sh].power = pow_;
            ShotPlan plan;
            plan_shot(&w, &w.tanks[sh], sp != 0, &plan);
            printf("P %d\n", plan.n);
            for (int f = 0; f < plan.n; f++) {
                FlightSpec *fs = &plan.f[f];
                printf("F %.17g %.17g %d %d %.17g %.17g %d\n", fs->trace.impact.x, fs->trace.impact.y, fs->trace.impact.kind, fs->trace.impact.tank,
                       fs->trace.time, fs->delay, fs->nevents);
                for (int e = 0; e < fs->nevents; e++) {
                    Ev *ev = &fs->events[e];
                    printf("E %.17g %.17g %d %d %.17g %d %d %.17g %d %.17g %d %d\n", ev->x, ev->y, ev->kind, ev->tank, ev->radius, ev->dmg, ev->mode,
                           ev->delay, ev->first, ev->pull, ev->multi, ev->big);
                }
            }
        }
        return 0;
    }
    if (!strcmp(argv[1], "ai")) {
        /* ai <map> <seed> <wind> <difficulty> <aiseed> <precision> <plans> [obs_dx obs_hit]... */
        int map = atoi(argv[2]); unsigned seed = (unsigned)strtoul(argv[3], 0, 10); double wind = atof(argv[4]);
        int diff = atoi(argv[5]); unsigned aseed = (unsigned)strtoul(argv[6], 0, 10); double prec = atof(argv[7]);
        int plans = atoi(argv[8]);
        PlayerSetup pl[2];
        mkplayer(&pl[0], 0, 0, 0, 0, 1.0, 0.0);
        mkplayer(&pl[1], 1, 3, 0, 1, 1.0, 0.0);
        pl[1].is_ai = true;
        static World w;
        world_init(&w, FIELD_W, FIELD_H, map, seed, pl, 2, wind);
        static AIController ai;
        ai_init(&ai, diff, aseed, prec);
        printf("G %.17g\n", ai.gravity_bias);
        for (int i = 0; i < plans; i++) {
            AimPlan pn;
            ai_plan(&ai, &w, 1, 0, &pn);
            printf("A %.17g %.17g %.17g %.17g %.17g %.17g\n", pn.angle, pn.power, pn.intended_rel, pn.intended_power, pn.think_time, pn.prob);
            printf("Q %s\n", pn.status);
            double dx = atof(argv[9 + i * 2]); int hit = atoi(argv[10 + i * 2]);
            Impact imp = {w.tanks[0].x + dx, 20.0, IMP_TERRAIN, -1};
            ai_observe(&ai, &pn, &imp, &w.tanks[0], hit != 0);
            printf("O %.17g %.17g\n", ai.gravity_bias, ai.correction);
            world_drift_wind(&w);
        }
        return 0;
    }
    if (!strcmp(argv[1], "battle")) {
        /* battle <seed> <wind> <diffA> <diffB> <designA> <ammoA> <designB> <ammoB> <ammo_fx> <special> <frames> [mode] */
        unsigned seed = (unsigned)strtoul(argv[2], 0, 10);
        static Settings st; settings_default(&st);
        st.wind = atof(argv[3]);
        static SaveData sv; save_new(&sv);
        PlayerSetup pl[2];
        mkplayer(&pl[0], 0, atoi(argv[6]), atoi(argv[7]), -1, 1.0, 0.0);
        mkplayer(&pl[1], 1, atoi(argv[8]), atoi(argv[9]), -1, 1.0, 0.0);
        pl[0].is_ai = pl[1].is_ai = true;
        pl[0].diff = (int8_t)atoi(argv[4]); pl[1].diff = (int8_t)atoi(argv[5]);
        static Session ss;
        session_init_match(&ss, pl, 2, MODE_SINGLE, DIFF_NORMAL, 2, "", atoi(argv[10]) != 0, atoi(argv[11]) != 0, false, false, -1, seed);
        int frames = atoi(argv[12]);
        static Battle b;
        int rounds = 0;
        battle_init(&b, &ss, &sv, &st);
        int last = -1;
        for (int f = 0; f < frames; f++) {
            battle_update(&b, 1.0 / 30.0);
            b.nfxq = 0;
            int ph = b.phase == PH_AI_THINKING && b.ai_pending ? -2 : b.phase;
            if (ph != last && ph != -2) {
                last = ph;
                if (ph == PH_AI_THINKING || ph == PH_DESTRUCTION) {
                    uint64_t hs = 0; for (int x = 0; x < b.w.width; x++) hs = hs * 31u + (uint64_t)(int64_t)b.w.terrain.heights[x];
                    char hb[24];
                    printf("S %d %d %.17g %s", ph, b.turn, b.w.wind, i64str((int64_t)hs, hb));
                    for (int i = 0; i < b.w.ntanks; i++) printf(" %.17g %.17g %d", b.w.tanks[i].x, b.w.tanks[i].y, b.w.tanks[i].hp);
                    if (ph == PH_AI_THINKING) printf(" %.17g %.17g", b.plan.angle, b.plan.power);
                    printf("\n");
                }
            }
            if (b.round_over) {
                printf("R %d %d %d\n", ss.round_no, ss.wins[0], ss.wins[1]);
                if (session_over(&ss) || ++rounds >= 3) break;
                session_advance(&ss);
                battle_init(&b, &ss, &sv, &st);
                last = -1;
            }
        }
        return 0;
    }
    return 1;
}
