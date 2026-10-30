/* Save migration test. Built twice: against the 1.5.0 core (git history) as the WRITER and against the current core as
 * the READER. The writer makes randomised saves (unlocks, loadouts, home screen, rules maps, custom tanks), reads each
 * back with its own code and prints what it holds BY NAME; the reader loads the same bytes with the new code and prints
 * the same description. tests/save_migration/run.sh compares the two.
 *   writer: roundtrip write <seed> <file>        reader: roundtrip read <file> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save.h"

static unsigned long long st = 88172645463325252ull;
static unsigned rnd(unsigned n) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (unsigned)(st % n); }

static void dump(const SaveData *s) {
    for (int u = 0; u < N_UIDS; u++)
        if ((s->granted[u >> 3] >> (u & 7)) & 1) printf("granted %s\n", UNLOCK_UIDS[u]);
    printf("completed %u missions %llu paid %llu tickets %u\n", s->completed, (unsigned long long)s->mission_completed,
           (unsigned long long)s->mission_paid, s->tickets_won);
    printf("martyr %d %d %d %d %d story %u coins %d credits %d\n", s->martyr_paid, s->martyr_won, s->martyr_seen, s->martyr_open,
           s->martyr_phase, s->story, s->coins, s->credits);
    for (int i = 0; i < 2; i++) {
        const Loadout *l = &s->loadouts[i];
        printf("loadout %d tank %s color %d ammo %s shot %d\n", i, l->tank >= 0 && l->tank < N_DESIGNS ? DESIGNS[l->tank].key : "?",
               l->tank_color, l->ammo >= 0 && l->ammo < N_AMMOS ? AMMOS[l->ammo].key : "?", l->shot_color);
    }
    printf("home %s difficulty %d\n", HOME_ORDER[s->settings.home_mode], s->settings.difficulty);
    for (int i = 0; i < 3; i++)
        printf("rules %d has %d map %s best %d\n", i, s->has_rules[i], s->rules[i].map < 0 ? "random" : MAPDEFS[s->rules[i].map].key, s->rules[i].best);
    for (int i = 0; i < CUSTOM_TANK_MAX; i++) {
        const CustomTank *c = &s->custom[i];
        if (!c->used) continue;
        printf("custom %d %s '%s' turret %s hull %s track %s barrel %s glow %s accent %s hover %d amb", i, c->id, c->name,
               TURRET_NAMES[c->turret], HULL_NAMES[c->hull], TRACK_NAMES[c->track], BARREL_NAMES[c->barrel], GLOW_NAMES[c->glow],
               COLOR_CHOICES[c->accent].name, c->hover);
        for (int k = 0; k < AMB_COUNT; k++) if ((c->ambient >> k) & 1) printf(" %s", AMBIENT_NAMES[k]);
        printf("\n");
    }
}

static uint8_t buf[16384];

int main(int argc, char **argv) {
    static SaveData s;
    if (argc == 4 && !strcmp(argv[1], "write")) {
        st += (unsigned long long)atoi(argv[2]) * 2654435761ull;
        save_new(&s);
        int density = 1 + rnd(4);                                     /* from a few unlocks to most of them */
        for (int u = 0; u < N_UNLOCKABLES; u++) if (rnd(5) < (unsigned)density) s.granted[u >> 3] |= (uint8_t)(1u << (u & 7));
        s.completed = rnd(1u << 20); s.mission_completed = ((unsigned long long)rnd(1u << 30) << 20) | rnd(1u << 20);
        s.tickets_won = (uint16_t)rnd(1024); s.martyr_phase = (uint8_t)rnd(3); s.martyr_seen = rnd(2); s.martyr_won = rnd(3) == 0;
        s.story = (uint16_t)rnd(1 << 11); s.coins = (int)rnd(500000); s.credits = (int)rnd(3000);
        for (int i = 0; i < 2; i++) {
            s.loadouts[i].tank = (int16_t)rnd(N_DESIGNS); s.loadouts[i].ammo = (int16_t)rnd(N_AMMOS_PUBLIC);
            s.loadouts[i].tank_color = (uint8_t)rnd(N_PAINTS); s.loadouts[i].shot_color = (uint8_t)rnd(N_PAINTS);
        }
        s.settings.home_mode = (int)rnd(N_HOME);
        for (int i = 0; i < 3; i++) { s.has_rules[i] = rnd(2); s.rules[i].map = (int16_t)((int)rnd(N_MAPS_ALL + 1) - 1); s.rules[i].best = (uint8_t)rnd(3); }
        int nc = (int)rnd(4);
        for (int i = 0; i < nc; i++) {
            CustomTank *c = &s.custom[i];
            c->used = true;
            snprintf(c->id, sizeof c->id, "c%d", i); snprintf(c->name, sizeof c->name, "TEST %d", i);
            c->turret = (uint8_t)rnd(TUR_COUNT); c->hull = (uint8_t)rnd(HUL_COUNT); c->track = (uint8_t)rnd(TRK_COUNT);
            c->barrel = (uint8_t)rnd(N_BARRELS); c->glow = (uint8_t)rnd(GLOW_COUNT); c->accent = (uint8_t)rnd(N_COLORS);
            c->ambient = 1u << rnd(AMB_COUNT); c->hover = rnd(2);
        }
        int n = save_serialize(&s, buf, sizeof buf);
        FILE *f = fopen(argv[3], "wb"); fwrite(buf, 1, (size_t)n, f); fclose(f);
        static SaveData back;
        if (!save_deserialize(&back, buf, n)) { printf("WRITER COULD NOT READ ITS OWN SAVE\n"); return 1; }
        dump(&back);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "read")) {
        FILE *f = fopen(argv[2], "rb"); int n = (int)fread(buf, 1, sizeof buf, f); fclose(f);
        if (!save_deserialize(&s, buf, n)) { printf("READ FAILED: %s\n", s.warning); return 1; }
        dump(&s);
        return 0;
    }
    return 2;
}
