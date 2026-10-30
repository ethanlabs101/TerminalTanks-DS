/* Boot, title, main menu, notices, how-to-play, settings, links and cheat codes (terminaltanks.py l.7509-7870,
 * 10948-10966, 12113-12172, 13102-13147). DS layout: the logo/backdrop owns the top screen, menus the touch screen. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenes.h"
#include "ui.h"

#define VERSION_STR "2.0.1"
#define CREDIT_NAME "ethanlabs101"
#define CREDIT_URL "github.com/ethanlabs101"
static const RGB GOLD = {255, 206, 100};

static void *scene_new(size_t size, const SceneVT *vt) {
    Scene *s = (Scene *)calloc(1, size);
    scene_base(s, vt);
    return s;
}
#define NEW(T, vt) ((T *)scene_new(sizeof(T), vt))

void backdrop_scene_update(Scene *s, float dt) { backdrop_update(s->home ? app_home_backdrop() : g_app.backdrop, dt); }
/* Backdrops are redrawn at a rate that fits the frame budget: a cheap one every frame, an expensive one every 2nd-4th
 * frame (its animation still advances every frame in update). In between, the cells of the last drawing are reused,
 * which also lets the app skip re-rendering a screen whose cells did not change. */
static Cell g_bd_cache[T_ROWS][T_COLS];
static const Backdrop *g_bd_owner;
static int g_bd_age, g_bd_every = 1;
static float g_bd_time;
float backdrop_frame_time(void) { return g_bd_owner ? g_bd_time : (float)g_app.time; }
void backdrop_scene_draw(Scene *s, Term *t) {
    Backdrop *b = s->home ? app_home_backdrop() : g_app.backdrop;
    if (b == g_bd_owner && g_bd_age < g_bd_every && !g_app.tr_phase) {
        memcpy(t->c, g_bd_cache, sizeof g_bd_cache);
        g_bd_age++;
        return;
    }
    uint32_t l0 = plat_lines();
    backdrop_draw(b, t, (float)g_app.time);
    uint32_t cost = plat_lines() - l0;
    memcpy(g_bd_cache, t->c, sizeof g_bd_cache);
    g_bd_owner = b;
    g_bd_age = 1;
    g_bd_time = (float)g_app.time;
    g_bd_every = cost < 90 ? 1 : cost < 200 ? 2 : cost < 330 ? 3 : 4;   /* lines; 1 frame = 526 */
}

/* ================================================================== boot (l.7538) */
typedef struct { Scene base; float t; } Boot;
static const char *const BOOT_LINES[9] = {"TERMINALTANKS // TACTICAL ARTILLERY SYSTEM", "author: " CREDIT_NAME " (" CREDIT_URL ")",
                                          "display link .............. online", "ballistics core ........... online",
                                          "terrain engine ............ online", "particle system ........... online",
                                          "fire-control AI ........... online", "tournament registry ....... online", "all systems nominal"};
static void boot_handle(Scene *s, const IEv *e) { (void)e; Boot *b = (Boot *)s; if (b->t < 9.0f) b->t = 9.0f; }
static void boot_update(Scene *s, float dt) {
    Boot *b = (Boot *)s;
    b->t += dt;
    if (b->t > 3.8f) app_goto(scene_title());
}
static void boot_draw(Scene *s, Term *t) {
    Boot *b = (Boot *)s;
    term_clear(t, P_BG);
    int x = 2, y = 2;
    int shown = (int)(b->t / 0.38f);
    for (int i = 0; i < 9 && i < shown; i++) {
        term_text(t, x, y + i * 2, BOOT_LINES[i], i == 0 ? P_PRIMARY : P_TEXT, NULL);
        if (i != 0 && i != 1) term_text(t, x + 41, y + i * 2, "[ OK ]", P_OK, NULL);
    }
    int w = 40;
    float frac = fminf(1.0f, fmaxf(0.0f, b->t / 3.4f));
    term_text(t, x, y + 20, "LOADING", P_MUTED, NULL);
    for (int i = 0; i < w; i++) term_put(t, x + 8 + i, y + 20, i < frac * w ? 0x2588 : 0x2591, i < frac * w ? P_PRIMARY : (RGB){36, 52, 62}, &P_BG);
    if (g_app.save.warning[0]) term_center(t, T_SPLIT + 10, g_app.save.warning, P_AMBER, NULL, 0, -1);
    if (!plat_storage_ok()) {
        term_center(t, T_SPLIT + 12, "NO SD CARD / FAT ACCESS:", P_AMBER, NULL, 0, -1);
        term_center(t, T_SPLIT + 13, "PROGRESS WILL NOT BE SAVED THIS SESSION", P_AMBER, NULL, 0, -1);
    }
}
static const SceneVT BOOT_VT = {.handle = boot_handle, .update = boot_update, .draw = boot_draw};
Scene *scene_boot(void) {
    Boot *b = NEW(Boot, &BOOT_VT);
    b->base.show_wallet = false;
    return &b->base;
}

/* ================================================================== home scenes (l.7574) */
typedef struct { Scene base; char note[64]; float note_t; MenuList menu; } Home;

static RGB theme_color(int home) {
    if (home == HOME_CLASSIC) return P_PRIMARY_DIM;
    if (home == HOME_MASTER) return GOLD;
    if (home == HOME_SECRET) return (RGB){110, 255, 150};
    return HOME_THEMES[home].accent;
}

static bool toggle_home(Home *h, Scene *(*again)(void)) {
    int modes[N_HOME];
    int n = save_home_modes(&g_app.save, modes);
    if (n < 2) {
        snprintf(h->note, sizeof h->note, "NO OTHER HOME SCREENS YET - BUY ONE IN THE SHOP");
        h->note_t = 3.0f;
        return false;
    }
    int cur = app_home_theme(), k = 0;
    for (int i = 0; i < n; i++) if (modes[i] == cur) k = i;
    g_app.settings.home_mode = modes[(k + 1) % n];
    plat_log("home -> %d", g_app.settings.home_mode);
    app_apply_settings();
    app_goto(again());
    return true;
}

static void home_update(Scene *s, float dt) {
    Home *h = (Home *)s;
    backdrop_scene_update(s, dt);
    h->note_t = fmaxf(0, h->note_t - dt);
}

static void draw_footer(Home *h, Term *t, int row) {
    if (h->note_t > 0) {
        term_center(t, row, h->note, P_AMBER, NULL, 0, -1);
        return;
    }
    int modes[N_HOME];
    int th = app_home_theme();
    char buf[96];
    if (save_home_modes(&g_app.save, modes) > 1) {
        snprintf(buf, sizeof buf, "[SELECT] HOME SCREEN: %s", HOME_ORDER[th]);
        term_center(t, row, buf, theme_color(th), NULL, 0, -1);
    } else {
        term_center(t, row, "[SELECT] MORE HOME SCREENS IN THE SHOP", P_MUTED, NULL, 0, -1);
        term_center(t, row + 1, "(Master screen: clear Level 20)", P_MUTED, NULL, 0, -1);
    }
}

static int draw_home_logo(Term *t, int y, bool tall) {
    int th = app_home_theme();
    float tm = backdrop_frame_time();   /* the shimmer steps with the backdrop, so unchanged frames are not re-rendered */
    if (th == HOME_MASTER) return ui_logo(t, LOGO_MASTER, y, tm, tall, th, false);
    if (th == HOME_SECRET) return ui_logo(t, LOGO_SECRET, y, tm, tall, th, false);
    if (th != HOME_CLASSIC) return ui_logo(t, LOGO_THEME, y, tm, tall, th, false);
    return ui_logo(t, LOGO_CLASSIC, y, tm, tall, th, !tall);
}

/* ---- title (l.7617) */
static void title_handle(Scene *s, const IEv *e) {
    Home *h = (Home *)s;
    if (e->action == A_OTHER && e->raw == K_SELECT) toggle_home(h, scene_title);
    else if (ev_confirm(e) || e->action == A_TOUCH || e->action == A_PAUSE) app_goto(scene_menu());
}
static void title_draw(Scene *s, Term *t) {
    Home *h = (Home *)s;
    backdrop_scene_draw(s, t);
    float tm = (float)g_app.time;
    int th = app_home_theme();
    int y = draw_home_logo(t, th == HOME_CLASSIC ? 4 : 3, true);
    if (th == HOME_CLASSIC) term_center(t, y + 1, "TACTICAL ARTILLERY SIMULATION", P_AMBER, NULL, 0, -1);
    RGB press = th == HOME_MASTER ? (RGB){255, 248, 220} : th == HOME_SECRET ? (RGB){200, 255, 215} :
                th == HOME_CLASSIC ? P_WHITE : cmix(HOME_THEMES[th].accent, P_WHITE, 0.6f);
    if ((int)(tm * 2) % 2 == 0) term_center(t, T_SPLIT + 8, "[ PRESS A ]", press, NULL, 0, -1);
    hot_add(0, T_SPLIT, T_COLS, T_SPLIT, 1);
    /* on an opaque strip: busy home screens (Eternal, Worldtree...) draw right behind these lines */
    term_center(t, T_SPLIT + 16, " v" VERSION_STR "  -  Nintendo DS port ", P_MUTED, &P_BG, 0, -1);
    term_center(t, T_SPLIT + 17, " created by " CREDIT_NAME " ", P_PRIMARY_DIM, &P_BG, 0, -1);
    term_center(t, T_SPLIT + 18, " " CREDIT_URL " ", P_PRIMARY_DIM, &P_BG, 0, -1);
    draw_footer(h, t, T_SPLIT + 21);
}
static const SceneVT TITLE_VT = {.handle = title_handle, .update = home_update, .draw = title_draw};
Scene *scene_title(void) {
    Home *h = NEW(Home, &TITLE_VT);
    h->base.home = true;
    return &h->base;
}

/* ---- main menu (l.7654) */
static const char *const MENU_ITEMS[] = {"SINGLE PLAYER", "TWO PLAYER", "LAN PLAY", "TOURNAMENT", "MISSIONS", "SHOP", "WORKSHOP",
                                         "HOW TO PLAY", "TUTORIAL", "PROFILES", "SETTINGS", "CHEAT CODES", "LINKS", "QUIT"};
#define HOT_MENU 100

static void mainmenu_handle(Scene *s, const IEv *e) {
    Home *h = (Home *)s;
    if (e->action == A_OTHER && e->raw == K_SELECT) { toggle_home(h, scene_menu); return; }
    if (e->action == A_BACK) { app_goto(scene_title()); return; }
    int c = menu_handle(&h->menu, e, HOT_MENU);
    if (c < 0) return;
    const char *label = h->menu.items[c];
    if (!strcmp(label, "SINGLE PLAYER")) app_goto(scene_rules(RK_SINGLE));
    else if (!strcmp(label, "TWO PLAYER")) app_goto(scene_rules(RK_TWO));
    else if (!strcmp(label, "LAN PLAY")) app_goto(scene_lan_menu());
    else if (!strcmp(label, "TOURNAMENT")) app_goto(scene_tournament(0));
    else if (!strcmp(label, "MISSIONS")) {
        if (save_missions_open(&g_app.save)) app_goto(scene_missions(0));
        else app_goto(scene_notice("MISSIONS LOCKED", "Missions open once you defeat the Master in Tournament mode (Level 20). "
                                                      "Your Tournament progress is on the TOURNAMENT screen."));
    } else if (!strcmp(label, "SHOP")) app_goto(scene_shop(0));
    else if (!strcmp(label, "WORKSHOP")) {
        if (save_workshop_open(&g_app.save)) app_goto(scene_workshop());
        else app_goto(scene_notice("WORKSHOP LOCKED", "The Tank Workshop opens once you defeat Level 5 in Tournament mode."));
    } else if (!strcmp(label, "TUTORIAL")) app_goto(scene_tutorial(false));
    else if (!strcmp(label, "PROFILES")) app_goto(scene_profiles());
    else if (!strcmp(label, "HOW TO PLAY")) app_goto(scene_howto());
    else if (!strcmp(label, "SETTINGS")) app_goto(scene_settings());
    else if (!strcmp(label, "CHEAT CODES")) app_goto(scene_cheats());
    else if (!strcmp(label, "LINKS")) app_goto(scene_links());
    else if (!strcmp(label, "QUIT")) g_app.running = false;
}

static void menu_draw_scene(Scene *s, Term *t) {
    Home *h = (Home *)s;
    backdrop_scene_draw(s, t);
    float tm = (float)g_app.time;
    int th = app_home_theme();
    int y = draw_home_logo(t, th == HOME_CLASSIC ? 4 : 3, true);
    if (th == HOME_CLASSIC) term_center(t, y + 1, "SELECT MISSION", P_PRIMARY_DIM, NULL, 0, -1);
    int my = T_SPLIT + 2, gap = 1, n = h->menu.n;
    int cx = t->cols / 2;
    if (th == HOME_MASTER) {
        int w = 38, hh = n * gap + 1, x0 = cx - w / 2;
        RGB bg = {10, 6, 22};
        term_fill(t, x0, my - 1, w, hh + 2, bg);
        int per = 2 * (w + hh);
        for (int i = 0; i < w; i++) {
            term_put(t, x0 + i, my - 1, 0x2501, chsv(0.1f + 0.12f * fsin((i / (float)per) * 6.2831853f * 2 + tm * 1.4f), 0.6f, 1.0f), &bg);
            int k = per / 2 - i;
            term_put(t, x0 + i, my + hh, 0x2501, chsv(0.1f + 0.12f * fsin((k / (float)per) * 6.2831853f * 2 + tm * 1.4f), 0.6f, 1.0f), &bg);
        }
        for (int j = 0; j < hh + 2; j++) {
            term_put(t, x0, my - 1 + j, 0x2503, chsv(0.1f + 0.1f * fsin(j * 0.4f + tm * 1.4f), 0.6f, 1.0f), &bg);
            term_put(t, x0 + w - 1, my - 1 + j, 0x2503, chsv(0.1f + 0.1f * fsin(j * 0.4f - tm * 1.4f), 0.6f, 1.0f), &bg);
        }
        term_put(t, x0, my - 1, 0x25C6, GOLD, &bg);
        term_put(t, x0 + w - 1, my - 1, 0x25C6, GOLD, &bg);
        term_put(t, x0, my + hh, 0x25C6, GOLD, &bg);
        term_put(t, x0 + w - 1, my + hh, 0x25C6, GOLD, &bg);
        for (int i = 0; i < n; i++) {
            int row = my + i * gap;
            if (i == h->menu.index) {
                for (int k = 0; k < w - 4; k++) {
                    float glow = 0.5f + 0.5f * fsin(tm * 5 - k * 0.15f);
                    RGB gb = cmix((RGB){60, 34, 8}, (RGB){120, 70, 16}, glow);
                    term_put(t, x0 + 2 + k, row, ' ', P_WHITE, &gb);
                }
                term_center(t, row, h->menu.items[i], (RGB){255, 248, 220}, NULL, x0, w);
                term_put(t, x0 + 3, row, 0x25C6, GOLD, NULL);
                term_put(t, x0 + w - 4, row, 0x25C6, GOLD, NULL);
            } else {
                term_center(t, row, h->menu.items[i], (RGB){178, 140, 110}, &bg, x0, w);
            }
            hot_add(x0, row, w, 1, HOT_MENU + i);
        }
    } else if (th == HOME_SECRET) {
        int w = 40, hh = n * gap + 1, x0 = cx - w / 2;
        RGB bg = {2, 18, 10}, sb = {8, 62, 30};
        term_box(t, x0, my - 1, w, hh + 2, TB_SINGLE, (RGB){50, 190, 95}, bg, "ROOT MENU", (RGB){150, 255, 180});
        for (int i = 0; i < n; i++) {
            int row = my + i * gap;
            if (i == h->menu.index) {
                char lab[48];
                snprintf(lab, sizeof lab, "> %s <", h->menu.items[i]);
                term_fill(t, x0 + 2, row, w - 4, 1, sb);
                term_center(t, row, lab, (RGB){200, 255, 215}, &sb, x0, w);
                if ((int)(tm * 3) % 2 == 0) term_put(t, x0 + w - 5, row, '_', (RGB){150, 255, 180}, &sb);
            } else {
                term_center(t, row, h->menu.items[i], (RGB){60, 150, 90}, &bg, x0, w);
            }
            hot_add(x0, row, w, 1, HOT_MENU + i);
        }
    } else if (th != HOME_CLASSIC) {
        const HomeTheme *ht = &HOME_THEMES[th];
        int w = 40, hh = n * gap + 1, x0 = cx - w / 2;
        RGB bg = cmix(ht->ground, ht->accent, 0.08f);
        term_box(t, x0, my - 1, w, hh + 2, TB_DOUBLE, cshade(ht->accent, 0.8f), bg, ht->name, ht->logo_a);
        RGB sel = cmix(bg, ht->accent, 0.28f);
        for (int i = 0; i < n; i++) {
            int row = my + i * gap;
            if (i == h->menu.index) {
                char lab[48];
                snprintf(lab, sizeof lab, "\xE2\x97\x86 %s \xE2\x97\x86", h->menu.items[i]);
                term_fill(t, x0 + 2, row, w - 4, 1, sel);
                term_center(t, row, lab, P_WHITE, &sel, x0, w);
            } else {
                term_center(t, row, h->menu.items[i], cshade(ht->accent, 0.62f), &bg, x0, w);
            }
            hot_add(x0, row, w, 1, HOT_MENU + i);
        }
    } else {
        menu_draw(t, &h->menu, cx, my, tm, gap, 30, HOT_MENU);
    }
    static const Keycap K[3] = {{"\xE2\x96\xB2\xE2\x96\xBC", "NAVIGATE"}, {"A", "SELECT"}, {"B", "BACK"}};
    ui_keycaps_center(t, T_ROWS - 5, K, 3, NULL, 0, t->cols);
    draw_footer(h, t, T_ROWS - 4);
    char pt[32], line[80];
    fmt_playtime(g_app.save.playtime, pt, sizeof pt);
    snprintf(line, sizeof line, "%s  \xC2\xB7  PLAYTIME %s", g_app.profiles.p[g_app.profiles.active].name, pt);
    term_text(t, 1, T_ROWS - 1, line, P_MUTED, NULL);
}
static const SceneVT MENU_VT = {.handle = mainmenu_handle, .update = home_update, .draw = menu_draw_scene};
Scene *scene_menu(void) {
    Home *h = NEW(Home, &MENU_VT);
    h->base.home = true;
    menu_init(&h->menu, MENU_ITEMS, 14);
    return &h->base;
}

/* ================================================================== notice (l.10948) */
typedef struct { Scene base; char title[48]; char text[320]; } Notice;
static void notice_handle(Scene *s, const IEv *e) {
    (void)s;
    if (ev_confirm(e) || e->action == A_BACK || e->action == A_TOUCH) app_goto(scene_menu());
}
static void notice_draw(Scene *s, Term *t) {
    Notice *n = (Notice *)s;
    backdrop_scene_draw(s, t);
    int x, y;
    ui_panel(t, 49, 12, n->title, -1, T_SPLIT, T_SPLIT, P_DANGER, &x, &y);
    char lines[6][64];
    int k = wrap_text(n->text, 43, lines, 6);
    for (int i = 0; i < k; i++) term_center(t, y + 2 + i, lines[i], P_TEXT, &P_PANEL, x, 49);
    if ((int)(g_app.time * 2) % 2 == 0) term_center(t, y + 9, "[ A ]", P_PRIMARY, &P_PANEL, x, 49);
    hot_add(x, y, 49, 12, 1);
}
static const SceneVT NOTICE_VT = {.handle = notice_handle, .update = backdrop_scene_update, .draw = notice_draw};
Scene *scene_notice(const char *title, const char *text) {
    Notice *n = NEW(Notice, &NOTICE_VT);
    snprintf(n->title, sizeof n->title, "%s", title);
    snprintf(n->text, sizeof n->text, "%s", text);
    return &n->base;
}

/* ================================================================== how to play (l.7756) */
typedef struct { const char *text; int kind; } HowLine;   /* kind 0 heading, 1 text */
static const HowLine HOW_P1[] = {
    {"OBJECTIVE", 0}, {"Destroy the enemy tank; every hit costs a heart. Terrain breaks.", 1},
    {"CONTROLS", 0}, {"\xE2\x97\x84 \xE2\x96\xBA rotate barrel (hold L or R for big steps)", 1},
    {"\xE2\x96\xB2 \xE2\x96\xBC firing power    A fire    START pause", 1}, {"Or use the touch controls under the battlefield.", 1},
    {"L/R + \xE2\x97\x84 \xE2\x96\xBA in pilot setup jumps 5 kits", 1},
    {"TIPS", 0}, {"WIND pushes every shell: read the arrows. The guide ignores wind.", 1},
    {"A blast right beside a tank still counts as a hit.", 1},
    {"TWO PLAYER / LAN", 0}, {"Pick BEST OF or FIRST TO n rounds and your hearts. LAN: one DS hosts, one joins over local wireless (no router); the host's rules apply.", 1},
    {"TOURNAMENT", 0}, {"Twenty levels, each opened by the one before. Beat a boss to unlock its exact tank + ammo. Usually 2 strikes; some levels bend the rules.", 1},
    {"MISSIONS", 0}, {"50 missions (open after you beat the Master): marathons, 2v1, UFO, co-op, rushes. Coins only. Co-op: local, LAN or an AI wingman. Grand prize: a UFO.", 1},
    {"SHOP", 0}, {"Spend coins (\xE2\x97\x89) and tournament credits (\xE2\x97\x88) on kits, maps and screens.", 1},
};
static const HowLine HOW_P2[] = {
    {"WHAT IT IS", 0},
    {"Ammo marked \xE2\x9C\xA6 in pilot setup has a real effect: it can bounce, split in flight, drop cluster bomblets, chain blasts, change gravity, speed, wind pull or blast size, and more. Plain ammo (STANDARD, TRACER, PLASMA...) only changes how shots look.", 1},
    {"WHEN IT WORKS", 0},
    {"Single Player: on by default - switch it ON/OFF in the Single Player rules. Two Player / LAN: when SPECIAL AMMO is ON in the match rules (the host decides in LAN). Terminal Tickets: always.", 1},
    {"WHEN IT DOES NOT", 0},
    {"Tournament and Missions: always off, so boss fights stay fair. Two Player / LAN with SPECIAL AMMO OFF. The 3-damage SPECIAL ATTACK (X) also skips the ammo's effect. SPECIAL ATTACK itself is a rule toggle in Single Player, Two Player and LAN.", 1},
    {"HOW YOU CAN TELL", 0},
    {"When it is off, the match banner says SPECIAL AMMO OFF and a note pops up over your tank on your first shot. The AI follows the same rules.", 1},
};
typedef struct { Scene base; int page; } HowTo;
static void howto_handle(Scene *s, const IEv *e) {
    HowTo *h = (HowTo *)s;
    if (e->action == A_BACK || (h->page == 1 && ev_confirm(e))) app_goto(scene_menu());
    else if (h->page == 0 && (ev_confirm(e) || e->action == A_RIGHT || (e->action == A_TOUCH && e->raw == 2))) h->page = 1;
    else if (e->action == A_LEFT || (e->action == A_TOUCH && e->raw == 3)) h->page = 0;
    else if (e->action == A_TOUCH && e->raw == 4) app_goto(scene_menu());
}
static void howto_draw(Scene *s, Term *t) {
    HowTo *h = (HowTo *)s;
    backdrop_scene_draw(s, t);
    const HowLine *L = h->page ? HOW_P2 : HOW_P1;
    int n = h->page ? (int)(sizeof HOW_P2 / sizeof HOW_P2[0]) : (int)(sizeof HOW_P1 / sizeof HOW_P1[0]);
    term_box(t, 1, 1, 49, 46, TB_DOUBLE, P_LINE, P_PANEL, h->page ? "HOW TO PLAY \xC2\xB7 SPECIAL AMMO" : "HOW TO PLAY", P_PRIMARY);
    int row = 3;
    for (int i = 0; i < n; i++) {
        char lines[8][64];
        int k = wrap_text(L[i].text, 44, lines, 8);
        if (L[i].kind == 0 && i) row++;
        if (L[i].kind == 0 && row == T_SPLIT - 1) row++;
        for (int j = 0; j < k; j++, row++) {
            if (row == T_SPLIT - 1 || row == T_SPLIT) row = T_SPLIT + 1;   /* keep text off the gap between the screens */
            term_text(t, 3, row, lines[j], L[i].kind == 0 ? P_AMBER : P_TEXT, &P_PANEL);
        }
    }
    if (h->page == 0) {
        ui_button(t, 13, T_ROWS - 3, "A / \xE2\x96\xBA NEXT: SPECIAL AMMO", P_PRIMARY_DIM, P_PANEL, 2);
    } else {
        ui_button(t, 6, T_ROWS - 3, "\xE2\x97\x84 PAGE 1", P_PRIMARY_DIM, P_PANEL, 3);
        ui_button(t, 30, T_ROWS - 3, "A BACK", P_PRIMARY_DIM, P_PANEL, 4);
    }
}
static const SceneVT HOWTO_VT = {.handle = howto_handle, .update = backdrop_scene_update, .draw = howto_draw};
Scene *scene_howto(void) { return &NEW(HowTo, &HOWTO_VT)->base; }

/* ================================================================== settings (l.7816) */
typedef struct { const char *label; double v; } Choice;
typedef struct { const char *name; int id; const char *desc; } SetRow;
enum { SR_DIFF, SR_WIND, SR_ANIM, SR_FX, SR_TRAIL, SR_PART, SR_GUIDE, SR_MAPS, SR_SOUND, SR_HOME, SR_N };
static const SetRow SET_ROWS[SR_N] = {
    {"AI DIFFICULTY", SR_DIFF, "How well the computer solves its firing problem"},
    {"WIND", SR_WIND, "Shifts every turn - read the arrows before you fire"},
    {"ANIMATION SPEED", SR_ANIM, "Speed of shells, blasts and AI decisions"},
    {"SCREEN EFFECTS", SR_FX, "Screen shake and blast flashes"},
    {"PROJECTILE TRAILS", SR_TRAIL, "Length of the fading tracer"},
    {"PARTICLE DENSITY", SR_PART, "Sparks, debris and smoke"},
    {"AIM GUIDE", SR_GUIDE, "Dotted NO-WIND preview - you add the wind"},
    {"MAP SELECTION", SR_MAPS, "Pick the opening battlefield or roll the dice"},
    {"SOUND", SR_SOUND, "A beep on impacts"},
    {"HOME SCREEN", SR_HOME, "Pick any home screen you own"},
};
static const Choice CH_DIFF[] = {{"EASY", DIFF_EASY}, {"NORMAL", DIFF_NORMAL}, {"HARD", DIFF_HARD}, {"MASTER", DIFF_MASTER}};
static const Choice CH_WIND[] = {{"OFF", 0.0}, {"LIGHT", 3.0}, {"NORMAL", 6.0}, {"STRONG", 10.0}};
static const Choice CH_ANIM[] = {{"SLOW", 0.7}, {"NORMAL", 1.0}, {"FAST", 1.6}};
static const Choice CH_BOOL[] = {{"OFF", 0}, {"ON", 1}};
static const Choice CH_TRAIL[] = {{"OFF", 0.0}, {"SHORT", 0.55}, {"LONG", 1.3}};
static const Choice CH_PART[] = {{"LOW", 0.45}, {"MEDIUM", 1.0}, {"HIGH", 1.8}};
static const Choice CH_GUIDE[] = {{"OFF", 0}, {"SHORT", 1}, {"FULL", 2}};
static const Choice CH_MAPS[] = {{"CHOOSE", 0}, {"RANDOM", 1}};
static const Choice CH_SOUND[] = {{"OFF", 0}, {"BEEP", 1}};

static int set_choices(int row, Choice *out) {
    const Choice *src = NULL;
    int n = 0;
    switch (row) {
    case SR_DIFF: src = CH_DIFF; n = save_master_unlocked(&g_app.save) ? 4 : 3; break;
    case SR_WIND: src = CH_WIND; n = 4; break;
    case SR_ANIM: src = CH_ANIM; n = 3; break;
    case SR_FX: src = CH_BOOL; n = 2; break;
    case SR_TRAIL: src = CH_TRAIL; n = 3; break;
    case SR_PART: src = CH_PART; n = 3; break;
    case SR_GUIDE: src = CH_GUIDE; n = 3; break;
    case SR_MAPS: src = CH_MAPS; n = 2; break;
    case SR_SOUND: src = CH_SOUND; n = 2; break;
    case SR_HOME: {
        int modes[N_HOME];
        n = save_home_modes(&g_app.save, modes);
        for (int i = 0; i < n; i++) out[i] = (Choice){HOME_ORDER[modes[i]], modes[i]};
        return n;
    }
    }
    for (int i = 0; i < n; i++) out[i] = src[i];
    return n;
}
static double set_get(int row) {
    Settings *s = &g_app.settings;
    switch (row) {
    case SR_DIFF: return s->difficulty;
    case SR_WIND: return s->wind;
    case SR_ANIM: return s->anim_speed;
    case SR_FX: return s->effects;
    case SR_TRAIL: return s->trail_life;
    case SR_PART: return s->particle_density;
    case SR_GUIDE: return s->aim_guide;
    case SR_MAPS: return s->random_maps;
    case SR_SOUND: return s->sound;
    default: return s->home_mode;
    }
}
static void set_put(int row, double v) {
    Settings *s = &g_app.settings;
    switch (row) {
    case SR_DIFF: s->difficulty = (int)v; break;
    case SR_WIND: s->wind = v; break;
    case SR_ANIM: s->anim_speed = v; break;
    case SR_FX: s->effects = v != 0; break;
    case SR_TRAIL: s->trail_life = v; break;
    case SR_PART: s->particle_density = v; break;
    case SR_GUIDE: s->aim_guide = (int)v; break;
    case SR_MAPS: s->random_maps = v != 0; break;
    case SR_SOUND: s->sound = v != 0; break;
    default: s->home_mode = (int)v; break;
    }
}
static int set_index(int row) {
    Choice ch[N_HOME];
    int n = set_choices(row, ch);
    double cur = set_get(row);
    for (int i = 0; i < n; i++) if (ch[i].v == cur) return i;
    return 0;
}

typedef struct { Scene base; int index; } SettingsScn;
#define HOT_SET 300
static void settings_handle(Scene *s, const IEv *e) {
    SettingsScn *ss = (SettingsScn *)s;
    int step = 0;
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    if (e->action == A_UP) ss->index = (ss->index + SR_N - 1) % SR_N;
    else if (e->action == A_DOWN) ss->index = (ss->index + 1) % SR_N;
    else if (e->action == A_LEFT) step = -1;
    else if (e->action == A_RIGHT || ev_confirm(e)) step = 1;
    else if (e->action == A_TOUCH && e->raw >= HOT_SET && e->raw < HOT_SET + SR_N) {
        if (ss->index == e->raw - HOT_SET) step = 1;
        ss->index = e->raw - HOT_SET;
    }
    if (step) {
        Choice ch[N_HOME];
        int n = set_choices(ss->index, ch);
        int i = (set_index(ss->index) + step + n) % n;
        set_put(ss->index, ch[i].v);
        app_apply_settings();
    }
}
static void settings_draw(Scene *s, Term *t) {
    SettingsScn *ss = (SettingsScn *)s;
    backdrop_scene_draw(s, t);
    int W = 49, h = T_SPLIT, x = 1, y = T_SPLIT;
    ui_logo(t, LOGO_CLASSIC, 4, (float)g_app.time, true, HOME_CLASSIC, false);
    term_box(t, x, y, W, h, TB_DOUBLE, P_LINE, P_PANEL, "SETTINGS", P_PRIMARY);
    for (int i = 0; i < SR_N; i++) {
        int ry = y + 1 + i * 2;
        bool sel = i == ss->index;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        term_fill(t, x + 2, ry, W - 4, 1, bg);
        term_text(t, x + 3, ry, SET_ROWS[i].name, sel ? P_WHITE : P_MUTED, &bg);
        Choice ch[N_HOME];
        set_choices(i, ch);
        const char *val = ch[set_index(i)].label;
        char buf[48];
        snprintf(buf, sizeof buf, sel ? "\xE2\x97\x84 %s \xE2\x96\xBA" : "%s", val);
        term_text(t, x + W - 3 - text_len(buf), ry, buf, sel ? P_PRIMARY : P_TEXT, &bg);
        hot_add(x + 2, ry, W - 4, 2, HOT_SET + i);
    }
    const char *desc = SET_ROWS[ss->index].desc;
    if (!save_master_unlocked(&g_app.save) && (ss->index == SR_DIFF || ss->index == SR_HOME)) desc = "MASTER unlocks after Tournament Level 10";
    term_center(t, y + h - 3, desc, P_MUTED, &P_PANEL, x, W);
    term_center(t, y + h - 2, "B  BACK", P_PRIMARY_DIM, &P_PANEL, x, W);
}
static const SceneVT SETTINGS_VT = {.handle = settings_handle, .update = backdrop_scene_update, .draw = settings_draw};
Scene *scene_settings(void) { return &NEW(SettingsScn, &SETTINGS_VT)->base; }

/* ================================================================== links (l.12113) */
typedef struct { Scene base; int row; char msg[64]; float msg_t; } Links;
/* 2.0.1 LinksScene, in its order */
#define N_LINKS 8
static const struct { const char *lab, *url; bool live; } LINKS[N_LINKS] = {
    {"WEBSITE", "ethanlabs101.github.io/terminaltanks-site", true}, {"ITCH.IO", "ethanlabs101.itch.io/terminal-tanks", true},
    {"TRAILER", "youtu.be/fp_Ca-Zxc98", true}, {"SOURCE CODE", "github.com/ethanlabs101/TerminalTanks", true},
    {"WEBSITE REPO", "github.com/ethanlabs101/terminaltanks-site", true}, {"DS PORT", "github.com/ethanlabs101/TerminalTanks-DS", true},
    {"ALL PROJECTS", "github.com/ethanlabs101", true}, {"STEAM", "COMING SOON", false}};
static void links_handle(Scene *s, const IEv *e) {
    Links *l = (Links *)s;
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    if (e->action == A_UP) l->row = (l->row + N_LINKS - 1) % N_LINKS;
    else if (e->action == A_DOWN) l->row = (l->row + 1) % N_LINKS;
    else if (ev_confirm(e) || (e->action == A_TOUCH && e->raw >= 10 && e->raw < 10 + N_LINKS)) {
        if (e->action == A_TOUCH) l->row = e->raw - 10;
        /* the DS has no web browser to hand the address to: show it for typing elsewhere */
        snprintf(l->msg, sizeof l->msg, "%s", LINKS[l->row].live ? "NO BROWSER ON THE DS - TYPE THE ADDRESS" : "NOT LIVE YET - CHECK BACK SOON");
        l->msg_t = 2.5f;
    }
}
static void links_update(Scene *s, float dt) {
    Links *l = (Links *)s;
    backdrop_scene_update(s, dt);
    l->msg_t = fmaxf(0, l->msg_t - dt);
}
static void links_draw(Scene *s, Term *t) {
    Links *l = (Links *)s;
    backdrop_scene_draw(s, t);
    int W = 49, x = 1, y = T_SPLIT - 6;
    term_box(t, x, y, W, 26, TB_DOUBLE, P_LINE, P_PANEL, "TERMINAL TANKS \xC2\xB7 FIND US", P_PRIMARY);
    term_center(t, y + 2, "Free, open source and built by one developer.", P_TEXT, &P_PANEL, x, W);
    term_center(t, y + 3, "Play it, read it, fork it, share it.", P_MUTED, &P_PANEL, x, W);
    for (int i = 0; i < N_LINKS; i++) {
        int ry = y + 5 + i * 2 + (i >= 3);                             /* the play links, a gap, then the code */
        bool sel = i == l->row;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        term_fill(t, x + 2, ry, W - 4, 2, bg);
        term_text(t, x + 3, ry, LINKS[i].lab, sel ? P_WHITE : P_MUTED, &bg);
        term_text(t, x + 5, ry + 1, LINKS[i].url, LINKS[i].live ? (sel ? GOLD : P_PRIMARY) : P_AMBER, &bg);
        hot_add(x + 2, ry, W - 4, 2, 10 + i);
    }
    bool m = l->msg_t > 0;
    term_center(t, y + 24, m ? l->msg : "More games from ethanlabs101 are on the way.", m ? P_AMBER : P_MUTED, &P_PANEL, x, W);
}
static const SceneVT LINKS_VT = {.handle = links_handle, .update = links_update, .draw = links_draw};
Scene *scene_links(void) { return &NEW(Links, &LINKS_VT)->base; }

/* ================================================================== cheat codes (l.13102) */
typedef struct { Scene base; char text[28]; char msg[96]; bool ok; float msg_t; Osk osk; } Cheats;
static void cheats_handle(Scene *s, const IEv *e) {
    Cheats *c = (Cheats *)s;
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    int k = osk_handle(&c->osk, e);
    int len = (int)strlen(c->text);
    if (k == K_BKSP) {
        if (len) c->text[len - 1] = 0;
    } else if (k == K_ENTER) {
        if (len) {
            int theme = -1;
            c->ok = redeem_cheat(&g_app.save, c->text, c->msg, sizeof c->msg, &theme);
            if (theme >= 0) {
                g_app.settings.home_mode = theme;
                app_apply_settings();
            } else {
                app_flush();
            }
            c->msg_t = 5.0f;
            c->text[0] = 0;
        }
    } else if (k > 0 && k < 128 && len < 24 && ((k >= '0' && k <= '9') || (k >= 'A' && k <= 'Z'))) {
        c->text[len] = (char)(k >= 'A' && k <= 'Z' ? k - 'A' + 'a' : k);
        c->text[len + 1] = 0;
    }
}
static void cheats_update(Scene *s, float dt) {
    Cheats *c = (Cheats *)s;
    backdrop_scene_update(s, dt);
    c->msg_t = fmaxf(0, c->msg_t - dt);
}
static void cheats_draw(Scene *s, Term *t) {
    Cheats *c = (Cheats *)s;
    backdrop_scene_draw(s, t);
    SaveData *sv = &g_app.save;
    int W = 49, x = 1, y = 1;
    term_box(t, x, y, W, 22, TB_DOUBLE, P_LINE, P_PANEL, "CHEAT CODES", P_PRIMARY);
    term_center(t, y + 2, "ENTER A CODE", P_AMBER, &P_PANEL, x, W);
    term_box(t, x + 4, y + 3, W - 8, 3, TB_SINGLE, P_LINE, P_PANEL_HI, "", P_PRIMARY);
    char buf[40];
    snprintf(buf, sizeof buf, "%s%s", c->text, (int)(g_app.time * 2) % 2 == 0 ? "\xE2\x96\x88" : " ");
    term_text(t, x + 6, y + 4, buf, P_WHITE, &P_PANEL_HI);
    if (c->msg_t > 0) term_wrap_center(t, y + 6, c->msg, W - 4, c->ok ? P_OK : P_DANGER, &P_PANEL, x, W, 2);
    int counts[6][2];
    content_counts(sv, counts);
    static const char *const LAB[6] = {"TANKS", "AMMO", "MAPS", "SCREENS", "PAINTS", "EXTRAS"};
    int row = y + 9;
    for (int k = 0; k < 6; k++, row++) {
        char v[48];
        snprintf(v, sizeof v, "%d/%d", counts[k][0], counts[k][1]);
        term_text(t, x + 4, row, LAB[k], P_MUTED, &P_PANEL);
        term_text(t, x + 13, row, v, P_TEXT, &P_PANEL);
    }
    int lv = 0, cl = 0, mo = 0, mc = 0, tw = 0;
    for (int n = 1; n <= N_LEVELS; n++) { lv += save_level_unlocked(sv, n); cl += (sv->completed >> (n - 1)) & 1; }
    for (int n = 1; n <= N_MISSIONS; n++) { mo += save_mission_unlocked(sv, n); mc += (int)((sv->mission_completed >> (n - 1)) & 1); }
    for (int i = 0; i < N_TICKETS; i++) tw += (sv->tickets_won >> i) & 1;
    char v[64];
    snprintf(v, sizeof v, "%d/%d OPEN \xC2\xB7 %d/%d CLEARED", lv, N_LEVELS, cl, N_LEVELS);
    term_text(t, x + 4, row, "LEVELS", P_MUTED, &P_PANEL);
    term_text(t, x + 13, row++, v, P_TEXT, &P_PANEL);
    snprintf(v, sizeof v, "%d/%d OPEN \xC2\xB7 %d/%d CLEARED", mo, N_MISSIONS, mc, N_MISSIONS);
    term_text(t, x + 4, row, "MISSION", P_MUTED, &P_PANEL);
    term_text(t, x + 13, row++, v, P_TEXT, &P_PANEL);
    snprintf(v, sizeof v, "%d/%d BOSSES", tw, N_TICKETS);
    term_text(t, x + 4, row, "TICKETS", P_MUTED, &P_PANEL);
    term_text(t, x + 13, row++, v, P_TEXT, &P_PANEL);
    term_text(t, x + 4, row, "MASTER", P_MUTED, &P_PANEL);
    term_text(t, x + 13, row, save_master_unlocked(sv) ? "DIFFICULTY UNLOCKED" : "DIFFICULTY LOCKED", P_TEXT, &P_PANEL);
    term_fill(t, 0, T_SPLIT, T_COLS, T_SPLIT, P_BG);
    osk_draw(t, &c->osk, T_SPLIT + 3, (float)g_app.time);
    static const Keycap K[4] = {{"A", "TYPE"}, {"X", "DELETE"}, {"START", "REDEEM"}, {"B", "BACK"}};
    ui_keycaps_center(t, T_ROWS - 3, K, 4, NULL, 0, T_COLS);
}
static const SceneVT CHEATS_VT = {.handle = cheats_handle, .update = cheats_update, .draw = cheats_draw};
Scene *scene_cheats(void) { return &NEW(Cheats, &CHEATS_VT)->base; }
