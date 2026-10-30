/* ProfilesScene (l.10163) and TutorialScene (l.9743). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "scenes.h"
#include "ui.h"
#include "render.h"

static const RGB GOLD = {255, 206, 100};

/* ================================================================== profiles */
typedef struct { Scene base; int sel, mode; char buf[16]; char msg[96]; bool confirm; Osk osk; } Profiles;
enum { PM_NONE = 0, PM_NEW, PM_RENAME };
static const char *const TIER_NAMES[5] = {"NEW GAME", "TOURNAMENT CLEARED", "MISSIONS CLEARED", "VAULT CLEARED", "LEGEND"};

static int prof_rows(void) { return g_app.profiles.n + (g_app.profiles.n < PROFILE_MAX ? 1 : 0); }

static RGB outline_color(int tier, int i, float t) {
    if (tier == 1) return cmix((RGB){190, 140, 40}, (RGB){255, 222, 120}, 0.5f + 0.5f * fsin(t * 1.2f + i * 0.1f));
    if (tier == 2) return cmix((RGB){14, 80, 36}, (RGB){110, 255, 150}, 0.5f + 0.5f * fsin(t * 3.0f + i * 0.05f));
    if (tier == 3) return cmix(void_tint(t), (RGB){10, 4, 28}, 0.5f + 0.5f * fsin(t * 1.4f + i * 0.12f));
    if (tier == 4) return (i + (int)(t * 6)) % 6 < 3 ? (RGB){255, 214, 90} : astral(t + i * 0.03f);
    return P_LINE;
}

#define HOT_PROF 1600
static void prof_handle(Scene *sc, const IEv *e) {
    Profiles *s = (Profiles *)sc;
    ProfileBook *b = &g_app.profiles;
    int rows = prof_rows();
    if (s->sel >= rows) s->sel = rows - 1;
    if (s->mode) {
        if (e->action == A_BACK) { s->mode = PM_NONE; return; }
        int k = osk_handle(&s->osk, e);
        int len = (int)strlen(s->buf);
        if (k == K_ENTER) {
            if (s->mode == PM_NEW) {
                int idx = app_new_profile(s->buf);
                if (idx >= 0) { app_switch_profile(idx); return; }
            } else if (s->sel < b->n) app_rename_profile(s->sel, s->buf);
            s->mode = PM_NONE;
        } else if (k == K_BKSP) { if (len) s->buf[len - 1] = 0; }
        else if (k == K_SPACE) { if (len < 14) { s->buf[len] = ' '; s->buf[len + 1] = 0; } }
        else if (k > 0 && k < 128 && len < 14 && (isalnum(k) || strchr("-_.", k))) { s->buf[len] = (char)toupper(k); s->buf[len + 1] = 0; }
        return;
    }
    if (e->action == A_BACK) { app_goto(scene_menu()); return; }
    bool confirm = ev_confirm(e);
    int raw = e->action == A_OTHER ? e->raw : 0;
    if (e->action == A_UP) { s->sel = (s->sel + rows - 1) % rows; s->confirm = false; }
    else if (e->action == A_DOWN) { s->sel = (s->sel + 1) % rows; s->confirm = false; }
    else if (e->action == A_TOUCH && e->raw >= HOT_PROF && e->raw < HOT_PROF + rows) {
        int r = e->raw - HOT_PROF;
        if (r == s->sel) confirm = true;
        else { s->sel = r; s->confirm = false; }
    } else if (e->action == A_TOUCH && e->raw == HOT_PROF + 10) raw = K_Y;
    else if (e->action == A_TOUCH && e->raw == HOT_PROF + 11) raw = K_X;
    if (confirm) {
        if (s->sel >= b->n) { s->mode = PM_NEW; s->buf[0] = 0; }
        else if (s->sel != b->active) app_switch_profile(s->sel);
        else snprintf(s->msg, sizeof s->msg, "THAT PROFILE IS ALREADY ACTIVE");
    } else if (raw == K_Y && s->sel < b->n) {
        s->mode = PM_RENAME;
        snprintf(s->buf, sizeof s->buf, "%s", b->p[s->sel].name);
    } else if (raw == K_X && s->sel < b->n) {
        if (b->n <= 1) snprintf(s->msg, sizeof s->msg, "YOU NEED AT LEAST ONE PROFILE");
        else if (!s->confirm) { s->confirm = true; snprintf(s->msg, sizeof s->msg, "PRESS X AGAIN TO DELETE THIS PROFILE AND ITS PROGRESS FOR GOOD"); }
        else {
            app_delete_profile(s->sel);
            s->confirm = false;
            s->sel = 0;
            snprintf(s->msg, sizeof s->msg, "PROFILE DELETED");
        }
    }
}

static void prof_draw(Scene *sc, Term *t) {
    Profiles *s = (Profiles *)sc;
    backdrop_scene_draw(sc, t);
    ProfileBook *b = &g_app.profiles;
    float tm = (float)g_app.time;
    int W = 51, x = 0, y = 0;
    term_box(t, x, y, W, 24, TB_DOUBLE, P_LINE, P_PANEL, "PROFILES", P_PRIMARY);
    term_wrap_center(t, y + 1, "Separate saves in one game. Switching saves your current profile first.", 47, P_MUTED, &P_PANEL, x, W, 2);
    static SaveData other;
    int rows = prof_rows();
    for (int i = 0; i < rows; i++) {
        bool sel = i == s->sel;
        int ry = y + 4 + i * 6, bx = x + 1, bw = W - 2;
        RGB bg = sel ? P_PANEL_HI : P_PANEL;
        term_fill(t, bx, ry, bw, 5, bg);
        hot_add(bx, ry, bw, 5, HOT_PROF + i);
        if (i >= b->n) {
            term_text(t, bx + 3, ry + 2, "+ NEW PROFILE", sel ? GOLD : P_MUTED, &bg);
            continue;
        }
        const SaveData *sv = &g_app.save;
        if (i != b->active) { app_load_profile_save(i, &other); sv = &other; }
        int tier = save_progress_tier(sv);
        int n = 0;
        for (int k = 0; k < bw; k++, n++) term_put(t, bx + k, ry, k == 0 ? 0x2554 : k == bw - 1 ? 0x2557 : 0x2550, outline_color(tier, n, tm), &bg);
        for (int k = 1; k < 5; k++, n++) term_put(t, bx + bw - 1, ry + k, k == 4 ? 0x255D : 0x2551, outline_color(tier, n, tm), &bg);
        for (int k = 1; k < bw; k++, n++) term_put(t, bx + bw - 1 - k, ry + 4, k == bw - 1 ? 0x255A : 0x2550, outline_color(tier, n, tm), &bg);
        for (int k = 1; k < 4; k++, n++) term_put(t, bx, ry + 4 - k, 0x2551, outline_color(tier, n, tm), &bg);
        bool act = i == b->active;
        char buf[96], pt[32], a[24], c[24];
        snprintf(buf, sizeof buf, "%s%s%s", sel ? "\xE2\x96\xB6 " : "  ", b->p[i].name, act ? "  \xE2\x98\x85" : "");
        term_text(t, bx + 2, ry + 1, buf, act ? GOLD : sel ? P_WHITE : P_TEXT, &bg);
        term_text(t, bx + bw - 2 - text_len(TIER_NAMES[tier]), ry + 1, TIER_NAMES[tier], outline_color(tier, (int)(tm * 10), tm), &bg);
        fmt_playtime(sv->playtime, pt, sizeof pt);
        fmt_commas(sv->coins, a, sizeof a);
        fmt_commas(sv->credits, c, sizeof c);
        snprintf(buf, sizeof buf, "%s  \xE2\x97\x89 %s  \xE2\x97\x88 %s", pt, a, c);
        term_text(t, bx + 3, ry + 2, buf, P_AMBER, &bg);
        int cnt[6][2];
        content_counts(sv, cnt);
        snprintf(buf, sizeof buf, "TANK %d/%d AMMO %d/%d MAP %d/%d SCR %d/%d", cnt[0][0], cnt[0][1], cnt[1][0], cnt[1][1], cnt[2][0], cnt[2][1], cnt[3][0], cnt[3][1]);
        term_text(t, bx + 3, ry + 3, buf, P_MUTED, &bg);
    }
    /* touch screen */
    term_fill(t, 0, T_SPLIT, T_COLS, T_SPLIT, P_BG);
    if (s->mode) {
        term_text(t, 1, T_SPLIT + 1, s->mode == PM_NEW ? "NAME THE NEW PROFILE:" : "RENAME PROFILE:", P_AMBER, NULL);
        char buf[32];
        snprintf(buf, sizeof buf, "%s%s", s->buf, (int)(tm * 2) % 2 == 0 ? "\xE2\x96\x88" : " ");
        term_fill(t, 1, T_SPLIT + 2, 30, 1, P_PANEL_HI);
        term_text(t, 2, T_SPLIT + 2, buf, P_WHITE, &P_PANEL_HI);
        osk_draw(t, &s->osk, T_SPLIT + 5, tm);
        static const Keycap K[3] = {{"A", "TYPE"}, {"START", "OK"}, {"B", "CANCEL"}};
        ui_keycaps_center(t, T_ROWS - 2, K, 3, NULL, 0, T_COLS);
        return;
    }
    if (s->msg[0]) term_wrap_center(t, T_SPLIT + 3, s->msg, 47, P_AMBER, NULL, 0, T_COLS, 2);
    ui_button(t, 4, T_SPLIT + 9, "A SWITCH / NEW", P_PRIMARY, P_PANEL_HI, HOT_PROF + s->sel);
    ui_button(t, 4, T_SPLIT + 12, "Y RENAME", P_PRIMARY, P_PANEL_HI, HOT_PROF + 10);
    ui_button(t, 4, T_SPLIT + 15, "X DELETE", P_DANGER, P_PANEL_HI, HOT_PROF + 11);
    static const Keycap K[2] = {{"\xE2\x96\xB2\xE2\x96\xBC", "CHOOSE"}, {"B", "BACK"}};
    ui_keycaps_center(t, T_ROWS - 2, K, 2, NULL, 0, T_COLS);
}
static const SceneVT PROF_VT = {.handle = prof_handle, .update = backdrop_scene_update, .draw = prof_draw};
Scene *scene_profiles(void) {
    Profiles *s = (Profiles *)calloc(1, sizeof(Profiles));
    scene_base(&s->base, &PROF_VT);
    s->sel = g_app.profiles.active;
    return &s->base;
}

/* ================================================================== tutorial menu */
extern const char *practice_challenge_name(int i);
extern const char *practice_lesson_name(int i);
typedef struct { Scene base; bool adv, page5; int sel; } Tut;
static int tut_items(const Tut *s, const char **out) {
    int n = 0;
    if (s->adv) {
        for (int i = 0; i < 3; i++) out[n++] = practice_challenge_name(i);
    } else {
        for (int i = 0; i < 4; i++) out[n++] = practice_lesson_name(i);
        out[n++] = "5  BOSS MECHANICS";
        out[n++] = "6  ADVANCED PRACTICE (OPTIONAL)";
    }
    out[n++] = "BACK";
    return n;
}
static void tut_handle(Scene *sc, const IEv *e) {
    Tut *s = (Tut *)sc;
    const char *items[8];
    int n = tut_items(s, items);
    if (s->page5) {
        if (ev_confirm(e) || e->action == A_BACK || e->action == A_TOUCH) {
            s->page5 = false;
            save_hear(&g_app.save, STORY_TUT5);
            app_flush();
        }
        return;
    }
    bool confirm = ev_confirm(e);
    if (e->action == A_BACK) { app_goto(s->adv ? scene_tutorial(false) : scene_menu()); return; }
    if (e->action == A_UP) s->sel = (s->sel + n - 1) % n;
    else if (e->action == A_DOWN) s->sel = (s->sel + 1) % n;
    else if (e->action == A_TOUCH && e->raw >= 1700 && e->raw < 1700 + n) {
        if (e->raw - 1700 == s->sel) confirm = true;
        else s->sel = e->raw - 1700;
    }
    if (!confirm) return;
    if (!strcmp(items[s->sel], "BACK")) app_goto(s->adv ? scene_tutorial(false) : scene_menu());
    else if (s->adv) app_goto(scene_practice(6, s->sel));
    else if (s->sel < 4) app_goto(scene_practice(s->sel + 1, -1));
    else if (s->sel == 4) s->page5 = true;
    else app_goto(scene_tutorial(true));
}
static void tut_draw(Scene *sc, Term *t) {
    Tut *s = (Tut *)sc;
    backdrop_scene_draw(sc, t);
    SaveData *sv = &g_app.save;
    if (s->page5) {
        term_box(t, 0, 0, 51, 48, TB_DOUBLE, P_LINE, P_PANEL, "TUTORIAL \xC2\xB7 5 BOSS MECHANICS", P_PRIMARY);
        typedef struct { char t[160]; RGB c; } L;
        static L lines[32];
        int n = 0;
#define ADD(str, col) do { snprintf(lines[n].t, sizeof lines[n].t, "%s", str); lines[n].c = col; n++; } while (0)
        ADD("Some opponents break the normal rules. Read what the game tells you:", P_TEXT);
        ADD("\xE2\x80\xA2 The ROUND BANNER names the fight and any special rule before you fire.", P_TEXT);
        ADD("\xE2\x80\xA2 Floating messages explain events: SHIELD ABSORBED, SHIELDS UP, VOLLEY!, CHARGING...", P_TEXT);
        ADD("\xE2\x80\xA2 Saucers hop between turns, raise shields as they are hurt and fire 3-shot volleys; direct hits count double.", P_TEXT);
        ADD("TOURNAMENT RULE TAGS (real examples from the level cards):", P_AMBER);
        bool seen[8] = {0};
        for (int l = 1; l <= N_LEVELS; l++) {
            int r = TOURNAMENT[l - 1].rule;
            if (r && !seen[r]) {
                seen[r] = true;
                char b[160];
                snprintf(b, sizeof b, "  %-13s %s", level_rule_tag(l), level_rule_text(l));
                ADD(b, GOLD);
            }
        }
        char b2[160];
        snprintf(b2, sizeof b2, "  STANDARD      %s", level_rule_text(2));
        ADD(b2, GOLD);
        ADD("EXERCISE: open TOURNAMENT and read the rule line on any level card before you start it.", P_AMBER);
        ADD("Boss kits are the kit you unlock by winning. Later bosses add their own mechanics - the banner always tells you.", P_TEXT);
#undef ADD
        int row = 2;
        for (int i = 0; i < n; i++) {
            char w[8][64];
            int k = wrap_text(lines[i].t, 47, w, 8);
            for (int j = 0; j < k; j++, row++) {
                if (row == T_SPLIT - 1 || row == T_SPLIT) row = T_SPLIT + 1;
                term_text(t, 2, row, w[j], lines[i].c, &P_PANEL);
            }
        }
        term_center(t, T_ROWS - 3, "[ A ] BACK", P_PRIMARY_DIM, &P_PANEL, 0, 51);
        return;
    }
    const char *items[8];
    int n = tut_items(s, items);
    int x, y;
    ui_panel(t, 51, 20, s->adv ? "TUTORIAL \xC2\xB7 ADVANCED PRACTICE" : "TUTORIAL", -1, T_SPLIT, T_SPLIT, P_LINE, &x, &y);
    term_wrap_center(t, y + 1, s->adv ? "Optional challenges for a keen eye." : "Optional. Practice range: no rewards, nothing can be lost.", 47,
                     P_MUTED, &P_PANEL, x, 51, 1);
    for (int i = 0; i < n; i++) {
        bool sel = i == s->sel;
        char b[80];
        snprintf(b, sizeof b, "%s%s", sel ? "\xE2\x96\xB6 " : "  ", items[i]);
        term_text(t, x + 3, y + 3 + i * 2, b, sel ? GOLD : P_TEXT, &P_PANEL);
        hot_add(x + 1, y + 3 + i * 2, 49, 2, 1700 + i);
        if (!s->adv && i < 6 && (sv->story & (1u << (STORY_TUT1 + i)))) term_text(t, x + 47, y + 3 + i * 2, "\xE2\x9C\x93", P_OK, &P_PANEL);
    }
    static const Keycap K[3] = {{"\xE2\x96\xB2\xE2\x96\xBC", "CHOOSE"}, {"A", "START"}, {"B", "BACK"}};
    ui_keycaps_center(t, y + 18, K, 3, &P_PANEL, x, 51);
}
static const SceneVT TUT_VT = {.handle = tut_handle, .update = backdrop_scene_update, .draw = tut_draw};
Scene *scene_tutorial(bool adv) {
    Tut *s = (Tut *)calloc(1, sizeof(Tut));
    scene_base(&s->base, &TUT_VT);
    s->adv = adv;
    return &s->base;
}
