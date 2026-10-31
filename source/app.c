#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "director.h"
#include "netplay.h"
#include "scenes.h"
#include "ui.h"

App g_app;

/* ------------------------------------------------------------------ scenes */
static void free_scene(Scene *s) { free(s); }

void scene_base(Scene *s, const SceneVT *vt) {
    s->vt = vt;
    s->overlay = s->ambient = s->home = false;
    s->show_wallet = true;
}
#define CALL(s, fn, ...) do { if ((s)->vt->fn) (s)->vt->fn(s, ##__VA_ARGS__); } while (0)
static void destroy(Scene *s) {
    if (s->vt->destroy) s->vt->destroy(s);
    else free_scene(s);
}

static void swap_to(Scene *s) {
    for (int i = 0; i < g_app.nstack; i++) CALL(g_app.stack[i], exit);
    for (int i = 0; i < g_app.nstack; i++) destroy(g_app.stack[i]);
    g_app.stack[0] = s;
    g_app.nstack = 1;
    CALL(s, enter);
}

void app_goto(Scene *s) {
    if (!g_app.nstack) { swap_to(s); return; }
    if (!g_app.tr_phase) {
        g_app.tr_scene = s;
        g_app.tr_phase = 1;
        g_app.tr_t = 0;
    } else {
        destroy(s);   /* a transition is already running (the original ignores a second goto too) */
    }
}
void app_goto_now(Scene *s) { swap_to(s); }
void app_push(Scene *s) {
    if (g_app.nstack >= 8) { destroy(s); return; }
    g_app.stack[g_app.nstack++] = s;
    CALL(s, enter);
}
void app_pop(void) {
    if (g_app.nstack > 1) {
        Scene *s = g_app.stack[--g_app.nstack];
        CALL(s, exit);
        destroy(s);
    }
}
Scene *app_top(void) { return g_app.nstack ? g_app.stack[g_app.nstack - 1] : NULL; }

/* ------------------------------------------------------------------ hotspots */
typedef struct { int x, y, w, h, id; } Hot;
static Hot g_hot[96];
static int g_nhot;
void hot_add(int x, int y, int w, int h, int id) {
    if (g_nhot < 96) g_hot[g_nhot++] = (Hot){x, y, w, h, id};
}
int app_hot_at(int cx, int cy) {
    for (int i = g_nhot - 1; i >= 0; i--) {
        Hot *h = &g_hot[i];
        if (cx >= h->x && cx < h->x + h->w && cy >= h->y && cy < h->y + h->h) return h->id;
    }
    return -1;
}

/* ------------------------------------------------------------------ saves & profiles (one file per profile) */
static uint8_t g_iobuf[24 * 1024];

static void profile_file(int idx, char *out, int n) { snprintf(out, (size_t)n, "profile%s.sav", g_app.profiles.p[idx].id); }

static void profiles_write(void) {
    ProfileBook *b = &g_app.profiles;
    char buf[512];
    int n = snprintf(buf, sizeof buf, "TTPROF1 %d %d\n", b->n, b->active);
    for (int i = 0; i < b->n; i++) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s\t%s\n", b->p[i].id, b->p[i].name);
    plat_save("profiles.txt", (const uint8_t *)buf, n);
}

static void profiles_read_defaults(void) {
    ProfileBook *b = &g_app.profiles;
    b->n = 1;
    b->active = 0;
    strcpy(b->p[0].id, "1");
    strcpy(b->p[0].name, "PROFILE 1");
}

static void profiles_read(void) {
    ProfileBook *b = &g_app.profiles;
    profiles_read_defaults();
    char buf[512];
    int n = plat_load("profiles.txt", (uint8_t *)buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    int cnt, act;
    if (sscanf(buf, "TTPROF1 %d %d", &cnt, &act) != 2 || cnt < 1 || cnt > PROFILE_MAX) return;
    char *p = strchr(buf, '\n');
    int got = 0;
    while (p && got < cnt) {
        p++;
        char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        if (!tab || !nl || tab > nl) break;
        int il = (int)(tab - p), nl2 = (int)(nl - tab - 1);
        if (il < 1 || il > 10 || nl2 < 1 || nl2 > 14) break;
        memcpy(b->p[got].id, p, (size_t)il);
        b->p[got].id[il] = 0;
        memcpy(b->p[got].name, tab + 1, (size_t)nl2);
        b->p[got].name[nl2] = 0;
        got++;
        p = nl;
    }
    if (got >= 1) {
        b->n = got;
        b->active = act >= 0 && act < got ? act : 0;
    }
}

bool app_load_profile_save(int idx, SaveData *out) {
    char f[32];
    profile_file(idx, f, sizeof f);
    save_new(out);
    int n = plat_load(f, g_iobuf, sizeof g_iobuf);
    if (n <= 0) return false;
    if (!save_deserialize(out, g_iobuf, n)) {
        /* never lose a save we cannot read: keep its bytes before anything is written over it */
        bool kept = plat_backup(f, "bak", g_iobuf, n);
        save_new(out);
        snprintf(out->warning, sizeof out->warning, kept ? "UNREADABLE SAVE KEPT AS %s.bak" : "SAVE FILE DAMAGED - STARTED FRESH", f);
        return false;
    }
    if (out->loaded_version == 1) plat_backup(f, "v1.bak", g_iobuf, n);   /* the 1.5.0 original, kept after migrating */
    return true;
}

void app_flush(void) {
    char f[32];
    profile_file(g_app.profiles.active, f, sizeof f);
    int n = save_serialize(&g_app.save, g_iobuf, sizeof g_iobuf);
    if (n > 0 && !plat_save(f, g_iobuf, n) && plat_storage_writable()) {
        g_app.save_fail_t = 5.0f;              /* say so instead of losing progress silently */
        plat_log("SAVE FAILED: %s", f);
    }
}

static void load_active(void) {
    app_load_profile_save(g_app.profiles.active, &g_app.save);
#ifdef TT_DEBUG_HOME   /* test builds only: everything unlocked, a fixed home screen */
    { char m[64]; int th; redeem_cheat(&g_app.save, "iamethanlabs101", m, sizeof m, &th); g_app.save.settings.home_mode = TT_DEBUG_HOME; }
#endif
#ifdef TT_DEBUG_TANK   /* test builds only: the player's loadout */
    g_app.save.loadouts[0].tank = TT_DEBUG_TANK;
#endif
#ifdef TT_DEBUG_AMMO
    g_app.save.loadouts[0].ammo = TT_DEBUG_AMMO;
#endif
#ifdef TT_DEBUG_PAINT
    g_app.save.loadouts[0].tank_color = TT_DEBUG_PAINT;
    g_app.save.loadouts[0].shot_color = TT_DEBUG_PAINT;
#endif
    custom_register_all(&g_app.save);
    save_apply_settings_guard(&g_app.save);
    g_app.settings = g_app.save.settings;
}

void app_apply_settings(void) {
    g_app.save.settings = g_app.settings;
    save_apply_settings_guard(&g_app.save);
    g_app.settings = g_app.save.settings;
    app_flush();
}

void app_switch_profile(int idx) {
    if (idx < 0 || idx >= g_app.profiles.n) return;
    app_flush();
    g_app.profiles.active = idx;
    profiles_write();
    load_active();
    app_goto(scene_menu());
}

int app_new_profile(const char *name) {
    ProfileBook *b = &g_app.profiles;
    if (b->n >= PROFILE_MAX) return -1;
    char id[4] = "1";
    for (int k = 1; k < 10; k++) {
        snprintf(id, sizeof id, "%d", k);
        bool used = false;
        for (int i = 0; i < b->n; i++) if (!strcmp(b->p[i].id, id)) used = true;
        if (!used) break;
    }
    strcpy(b->p[b->n].id, id);
    char nm[16];
    int l = 0;
    while (*name == ' ') name++;
    for (; name[l] && l < 14; l++) nm[l] = name[l];
    while (l > 0 && nm[l - 1] == ' ') l--;
    nm[l] = 0;
    if (!l) snprintf(nm, sizeof nm, "PROFILE %s", id);
    strcpy(b->p[b->n].name, nm);
    b->n++;
    profiles_write();
    return b->n - 1;
}

void app_rename_profile(int idx, const char *name) {
    while (*name == ' ') name++;
    if (!*name || idx < 0 || idx >= g_app.profiles.n) return;
    char nm[16];
    int l = 0;
    for (; name[l] && l < 14; l++) nm[l] = name[l];
    while (l > 0 && nm[l - 1] == ' ') l--;
    nm[l] = 0;
    strcpy(g_app.profiles.p[idx].name, nm);
    profiles_write();
}

void app_delete_profile(int idx) {
    ProfileBook *b = &g_app.profiles;
    if (b->n <= 1 || idx < 0 || idx >= b->n) return;
    bool was_active = idx == b->active;
    char f[32], path[64];
    profile_file(idx, f, sizeof f);
    snprintf(path, sizeof path, "%s/%s", plat_data_dir(), f);
    for (int i = idx; i < b->n - 1; i++) b->p[i] = b->p[i + 1];
    b->n--;
    if (was_active) b->active = 0;
    else if (b->active > idx) b->active--;
    profiles_write();
    remove(path);
    if (was_active) load_active();
}

/* ------------------------------------------------------------------ home screens */
int app_home_theme(void) {
    int m = g_app.settings.home_mode;
    return save_home_unlocked(&g_app.save, m) ? m : HOME_CLASSIC;
}

void app_feedback(int kind) {
    (void)kind;
    static double last = -1;
    int mode, mv, sv;
    settings_audio(&g_app.settings, &mode, &mv, &sv);
    if (mode == SND_BEEP && g_app.time - last > 0.15) {
        last = g_app.time;
        plat_beep(kind == 1 ? 2 : 1);
    }
}

/* ------------------------------------------------------------------ frame */
uint32_t g_prof[6];
static Term g_term;
static Term g_prev;
static bool g_prev_valid;

static void events_from_input(const Input *in, IEv *evs, int *nev) {
    int n = 0;
    bool coarse = (in->held & (BTN_L | BTN_R)) != 0;
    static const struct { uint32_t b; int a; int raw; } MAP[] = {
        {BTN_UP, A_UP, 0}, {BTN_DOWN, A_DOWN, 0}, {BTN_LEFT, A_LEFT, 0}, {BTN_RIGHT, A_RIGHT, 0},
    };
    for (unsigned i = 0; i < 4; i++)
        if (in->repeat & MAP[i].b) evs[n++] = (IEv){MAP[i].a, coarse, 0, 0, 0};
    if (in->down & BTN_A) evs[n++] = (IEv){A_CONFIRM, coarse, K_ENTER, 0, 0};
    if (in->down & BTN_B) evs[n++] = (IEv){A_BACK, false, 0, 0, 0};
    if (in->down & BTN_START) evs[n++] = (IEv){A_PAUSE, false, 0, 0, 0};
    if (in->down & BTN_X) evs[n++] = (IEv){A_OTHER, false, K_X, 0, 0};
    if (in->down & BTN_Y) evs[n++] = (IEv){A_OTHER, false, K_Y, 0, 0};
    if (in->down & BTN_SELECT) evs[n++] = (IEv){A_OTHER, false, K_SELECT, 0, 0};
    if (in->down & BTN_L) evs[n++] = (IEv){A_OTHER, false, K_L, 0, 0};
    if (in->down & BTN_R) evs[n++] = (IEv){A_OTHER, false, K_R, 0, 0};
    if (in->touch_down) {
        int cx = in->tx / CELL_W, cy = T_SPLIT + in->ty / CELL_H;
        evs[n++] = (IEv){A_TOUCH, false, app_hot_at(cx, cy), cx, cy};
    }
    *nev = n;
}

Input g_input;   /* the raw state, for scenes that read held buttons (battle aiming) */

void app_step(float dt) {
    IEv evs[16];
    int nev = 0;
    uint32_t q0 = plat_lines();
    events_from_input(&g_input, evs, &nev);
    g_app.time += dt;
    g_app.save.playtime += dt < 0.5f ? dt : 0.5f;
    g_app.pt_flush += dt;
    if (g_app.pt_flush >= 60.0f) {
        g_app.pt_flush = 0;
        app_flush();
    }
    if (g_app.tr_phase) {
        g_app.tr_t += dt;
        float prog = g_app.tr_t / (g_app.tr_phase == 1 ? 0.22f : 0.30f);
        if (g_app.tr_phase == 1 && prog >= 1) {
            swap_to(g_app.tr_scene);
            g_app.tr_scene = NULL;
            g_app.tr_phase = 2;
            g_app.tr_t = 0;
        } else if (g_app.tr_phase == 2 && prog >= 1) {
            g_app.tr_phase = 0;
        }
    } else {
        for (int i = 0; i < nev && !g_app.tr_phase; i++) {
            Scene *top = app_top();
            director_ui_sound(top, &evs[i]);
            if (top && top->vt->handle) top->vt->handle(top, &evs[i]);
        }
    }
    director_frame(dt);
    net_frame();                                   /* the wireless link is kept alive whatever screen is up */
    uint32_t q1 = plat_lines();
    g_nhot = 0;
    term_clear(&g_term, P_BG);
    Scene *top = app_top();
    int base = g_app.nstack - 1;
    if (top) {
        g_prof[0] = plat_lines();
        CALL(top, update, dt);
        g_prof[1] = plat_lines();
        top = app_top();
        for (int i = 0; i < g_app.nstack - 1; i++)
            if (g_app.stack[i]->ambient) CALL(g_app.stack[i], update_ambient, dt);
        base = g_app.nstack - 1;
        while (base > 0 && g_app.stack[base]->overlay) base--;
        for (int i = base; i < g_app.nstack; i++) CALL(g_app.stack[i], draw, &g_term);
        g_prof[2] = plat_lines();
        bool tr_hide = g_app.tr_phase == 1 && g_app.tr_t / 0.22f > 0.9f;
        if (g_app.stack[base]->show_wallet && !tr_hide) {
            g_app.wallet_flash = g_app.wallet_flash - dt > 0 ? g_app.wallet_flash - dt : 0;
            ui_wallet(&g_term, &g_app.save, g_app.wallet_flash);
        }
    }
    director_draw_popup(&g_term);
    const char *nt = net_trouble_text();
    if (nt) {                                      /* the link is being repaired: say so on top of whatever is showing */
        RGB bg = {40, 30, 6};
        term_fill(&g_term, 0, T_SPLIT - 1, T_COLS, 1, bg);
        term_center(&g_term, T_SPLIT - 1, nt, P_AMBER, &bg, 0, T_COLS);
    }
    if (g_app.save_fail_t > 0) {
        g_app.save_fail_t -= dt;
        RGB bg = {60, 10, 10};
        term_fill(&g_term, 0, T_SPLIT, T_COLS, 1, bg);
        term_center(&g_term, T_SPLIT, "COULD NOT SAVE - CHECK THE SD CARD", P_AMBER, &bg, 0, T_COLS);
    }
    /* render: pixel layers, cells, overlays */
    Surf *ts = plat_top(), *bs = plat_bottom();
    bool px = false;
    for (int i = base; i < g_app.nstack; i++) if (g_app.stack[i]->vt->draw_px || g_app.stack[i]->vt->draw_over) px = true;
    bool fading = g_app.tr_phase != 0;
    bool same_top = !px && !fading && g_prev_valid && !memcmp(&g_prev.c[0], &g_term.c[0], sizeof(Cell) * T_COLS * T_SPLIT) && !ui_logo_changed();
    bool same_bot = !fading && g_prev_valid && !memcmp(&g_prev.c[T_SPLIT], &g_term.c[T_SPLIT], sizeof(Cell) * T_COLS * T_SPLIT);
    for (int i = base; i < g_app.nstack; i++)
        if (g_app.stack[i]->vt->draw_px) g_app.stack[i]->vt->draw_px(g_app.stack[i], ts, bs);
    g_prof[3] = plat_lines();
    if (!same_top) { term_render(&g_term, ts, 0); plat_top_dirty(); }
    g_prof[4] = plat_lines();
    if (!same_bot) { term_render(&g_term, bs, T_SPLIT); plat_bottom_dirty(); }
    g_prof[5] = plat_lines();
    if (same_top) ui_logo_keep();
    else ui_logo_flush(ts);
    uint32_t q2 = plat_lines();
    for (int i = base; i < g_app.nstack; i++)
        if (g_app.stack[i]->vt->draw_over) g_app.stack[i]->vt->draw_over(g_app.stack[i], ts, bs);
    uint32_t q3 = plat_lines();
    if (q3 - q0 > 1000) plat_log("slow step: ev %lu pre %lu upd %lu draw %lu px %lu rt %lu rb %lu logo %lu over %lu", (unsigned long)(q1-q0), (unsigned long)(g_prof[0]-q1), (unsigned long)(g_prof[1]-g_prof[0]), (unsigned long)(g_prof[2]-g_prof[1]), (unsigned long)(g_prof[3]-g_prof[2]), (unsigned long)(g_prof[4]-g_prof[3]), (unsigned long)(g_prof[5]-g_prof[4]), (unsigned long)(q2-g_prof[5]), (unsigned long)(q3-q2));
    memcpy(&g_prev, &g_term, sizeof g_term);
    g_prev_valid = !fading;
    if (fading) {
        float p = g_app.tr_t / (g_app.tr_phase == 1 ? 0.22f : 0.30f);
        if (p > 1) p = 1;
        for (int r = 0; r < T_ROWS; r++) {
            float k = g_app.tr_phase == 1 ? 1 - (p * 1.6f - r / (float)T_ROWS * 0.6f) : p * 1.6f - r / (float)T_ROWS * 0.6f;
            k = k < 0 ? 0 : (k > 1 ? 1 : k);
            if (k >= 0.999f) continue;
            Surf *s = r < T_SPLIT ? ts : bs;
            Surf band = {s->px + ((r % T_SPLIT) * CELL_H) * s->stride, s->w, CELL_H, s->stride};
            surf_dim(&band, k);
        }
        plat_top_dirty();
        plat_bottom_dirty();
    }
}

/* boot progress on the top screen, shown before anything touches the card, so a problem is never a blank screen */
static void boot_status(const char *line1, const char *line2) {
    term_clear(&g_term, P_BG);
    term_center(&g_term, 9, "TERMINAL TANKS", P_PRIMARY, NULL, 0, -1);
    term_center(&g_term, 12, line1, P_TEXT, NULL, 0, -1);
    if (line2) term_center(&g_term, 14, line2, P_MUTED, NULL, 0, -1);
    term_center(&g_term, T_SPLIT + 20, "HOLD L+R WHILE STARTING: SAFE START (NO SAVES)", P_MUTED, NULL, 0, -1);
    term_render(&g_term, plat_top(), 0);
    term_render(&g_term, plat_bottom(), T_SPLIT);
    plat_show_now();
}

void app_init(void) {
    memset(&g_app, 0, sizeof g_app);
    g_app.running = true;
    boot_status("STARTING...", NULL);
    uint32_t k = plat_keys_now();
    bool safe = (k & (BTN_L | BTN_R)) == (BTN_L | BTN_R);
    boot_status(safe ? "SAFE START" : "READING SD CARD...", safe ? "saves will not be loaded or written this session" : NULL);
    plat_init_storage(safe);
    rng_seed(&g_app.rng, plat_entropy());
    boot_status("LOADING SOUND...", NULL);
    audio_init();
    if (!safe) {
        boot_status("LOADING PROFILE...", NULL);
        profiles_read();
        load_active();
    } else {
        profiles_read_defaults();
        save_new(&g_app.save);
        snprintf(g_app.save.warning, sizeof g_app.save.warning, "SAFE START - PROGRESS IS NOT SAVED");
        g_app.settings = g_app.save.settings;
    }
    g_app.backdrop = backdrop_new_ambient();
}

int main(void) {
    plat_init();
    term_init();
    app_init();
    app_goto_now(scene_boot());
    while (1) {
        plat_input(&g_input);
        float dt = (float)plat_frame_dt();
        uint32_t l0 = plat_lines();
        app_step(dt);
        uint32_t used = plat_lines() - l0;
        static uint32_t sum, mx, n;
        sum += used; if (used > mx) mx = used;
        if (++n == 60) {          /* profiling line on the debug console (no$gba / melonDS): average and worst frame cost */
            plat_log("perf avg %lu us max %lu us dt %d ms | upd %lu draw %lu px %lu rtop %lu rbot %lu lines", (unsigned long)(sum / n * 6356 / 100),
                     (unsigned long)(mx * 6356 / 100), (int)(dt * 1000), (unsigned long)(g_prof[1] - g_prof[0]), (unsigned long)(g_prof[2] - g_prof[1]),
                     (unsigned long)(g_prof[3] - g_prof[2]), (unsigned long)(g_prof[4] - g_prof[3]), (unsigned long)(g_prof[5] - g_prof[4]));
            sum = mx = n = 0;
        }
        plat_sound_tick();
        plat_present();
        if (!g_app.running) {
            app_flush();
            g_app.running = true;
            app_goto(scene_title());   /* the DS has no "quit to shell": QUIT saves and returns to the title */
        }
    }
}
