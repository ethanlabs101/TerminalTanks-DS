/* The JUKEBOX (tanks/scenes/jukebox.py JukeboxScene): every unlocked song, grouped like the PC's, with the
 * analyser. Top screen: NOW PLAYING; touch screen: the shelf. The PC's 3-band EQ is not on the DS (the music is
 * streamed straight to the sound hardware). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenes.h"
#include "ui.h"
#include "director.h"

#define MAX_ROWS (N_SONGS + N_SONG_GROUPS)
#define VIS 18
#define HOT_SONG 4000
typedef struct { short song; uint8_t group, got, total; } JRow;   /* song < 0: a group header */
typedef struct {
    Scene base;
    JRow rows[MAX_ROWS];
    int n, sel, top, playing;
    bool loop_one;
    float bars[16];
} Jukebox;

static void jb_step(Jukebox *j, int d) {
    for (int k = 0; k < j->n; k++) {
        j->sel = (j->sel + d + j->n) % j->n;
        if (j->rows[j->sel].song >= 0) return;
    }
}
static void jb_group(Jukebox *j, int d) {   /* first song of the next / previous group */
    int g = j->rows[j->sel].group;
    for (int k = 1; k <= j->n; k++) {
        int i = (j->sel + d * k + j->n * k) % j->n;
        if (j->rows[i].song >= 0 && j->rows[i].group != g) {
            if (d < 0) while (i > 0 && j->rows[i - 1].song >= 0 && j->rows[i - 1].group == j->rows[i].group) i--;
            j->sel = i;
            return;
        }
    }
}
static void jb_play(Jukebox *j) {
    if (j->rows[j->sel].song < 0) return;
    j->playing = j->rows[j->sel].song;
    audio_loop(j->loop_one);
}

static void jb_handle(Scene *sc, const IEv *e) {
    Jukebox *j = (Jukebox *)sc;
    if (e->action == A_BACK) { audio_loop(true); app_goto(scene_menu()); return; }
    if (!j->n) return;
    if (e->action == A_UP || e->action == A_DOWN) {
        for (int k = 0; k < (e->coarse ? 10 : 1); k++) jb_step(j, e->action == A_UP ? -1 : 1);
    } else if (e->action == A_LEFT || e->action == A_RIGHT) {
        jb_group(j, e->action == A_LEFT ? -1 : 1);
    } else if (e->action == A_OTHER && e->raw == K_Y) {
        j->loop_one = !j->loop_one;
        audio_loop(j->loop_one);
    } else if (e->action == A_TOUCH && e->raw >= HOT_SONG && e->raw < HOT_SONG + j->n) {
        int i = e->raw - HOT_SONG;
        if (i == j->sel) jb_play(j);
        else if (j->rows[i].song >= 0) j->sel = i;
    } else if (e->action == A_TOUCH && e->raw == 1) {
        j->loop_one = !j->loop_one;
        audio_loop(j->loop_one);
    } else if (ev_confirm(e)) {
        jb_play(j);
    }
}

static void jb_update(Scene *sc, float dt) {
    Jukebox *j = (Jukebox *)sc;
    backdrop_scene_update(sc, dt);
    uint8_t sp[16];
    audio_spectrum(sp);
    for (int i = 0; i < 16; i++) {
        float v = sp[i];
        j->bars[i] += (v - j->bars[i]) * (dt * (v > j->bars[i] ? 14 : 5) < 1 ? dt * (v > j->bars[i] ? 14 : 5) : 1);
    }
    if (audio_take_ended() && j->playing >= 0 && !j->loop_one) {   /* AUTO-PLAY: the next song on the shelf */
        int at = -1;
        for (int i = 0; i < j->n; i++) if (j->rows[i].song == j->playing) at = i;
        for (int k = 1; k <= j->n && at >= 0; k++) {
            int i = (at + k) % j->n;
            if (j->rows[i].song >= 0) { j->playing = j->rows[i].song; break; }
        }
    }
}

static int jb_music(Scene *sc, int *mood) {
    Jukebox *j = (Jukebox *)sc;
    *mood = MOOD_MENU;
    return j->playing;
}

static void jb_draw(Scene *sc, Term *t) {
    Jukebox *j = (Jukebox *)sc;
    backdrop_scene_draw(sc, t);
    /* ---- top screen: NOW PLAYING */
    int W = 51, x = 0, y = 0, H = T_SPLIT;
    term_box(t, x, y, W, H, TB_DOUBLE, P_LINE, P_PANEL, "NOW PLAYING", P_PRIMARY);
    int cur = j->playing >= 0 ? j->playing : audio_current_song();
    int mode, mv, sv;
    settings_audio(&g_app.settings, &mode, &mv, &sv);
    if (mode != SND_ON || !audio_ready()) {
        term_center(t, y + 10, "Audio is off. Turn AUDIO on in Settings.", P_AMBER, &P_PANEL, x, W);
    } else if (cur >= 0 && cur < N_SONGS) {
        term_center(t, y + 2, SONG_NAMES[cur], P_GOLD, &P_PANEL, x, W);
        char dl[3][64];
        int nd = wrap_text(SONG_DESC[cur], W - 6, dl, 3);
        for (int k = 0; k < nd; k++) term_center(t, y + 3 + k, dl[k], P_MUTED, &P_PANEL, x, W);
        term_center(t, y + 7, SONG_INFO[cur], P_TEXT, &P_PANEL, x, W);
        int pos, dur;
        audio_position(&pos, &dur);
        int bw = 36, nfill = dur > 0 ? bw * (pos > dur ? dur : pos) / dur : 0;
        char bar[3 * 40 + 1];
        int o = 0;
        for (int k = 0; k < bw; k++) {
            const char *c = k < nfill ? "\xE2\x96\x88" : "\xE2\x96\x91";
            memcpy(bar + o, c, 3);
            o += 3;
        }
        bar[o] = 0;
        term_text(t, x + 4, y + 9, bar, P_PRIMARY, &P_PANEL);
        char tm[16];
        snprintf(tm, sizeof tm, "%d:%02d", pos / 60, pos % 60);
        term_text(t, x + 4 + bw + 2, y + 9, tm, P_MUTED, &P_PANEL);
    }
    /* the analyser: 16 bands, 8 rows */
    int base = y + 19;
    for (int i = 0; i < 16; i++) {
        int h = (int)(j->bars[i] + 0.5f);
        for (int r = 0; r < 8; r++) {
            RGB col = r < h ? (r < 4 ? (RGB){60, 220, 160} : r < 6 ? (RGB){255, 214, 90} : (RGB){255, 80, 90}) : (RGB){32, 36, 52};
            term_text(t, x + 2 + i * 3, base - r, "\xE2\x96\x86\xE2\x96\x86", col, &P_PANEL);
        }
    }
    term_text(t, x + 2, base + 1, "55Hz", P_MUTED, &P_PANEL);
    term_text(t, x + W - 7, base + 1, "7kHz", P_MUTED, &P_PANEL);
    /* ---- touch screen: the shelf */
    int by = T_SPLIT, bw2 = 51;
    term_box(t, 0, by, bw2, 21, TB_DOUBLE, P_LINE, P_PANEL, "JUKEBOX", P_PRIMARY);
    if (!j->n) {
        term_center(t, by + 9, "No songs yet - play to fill the shelf.", P_MUTED, &P_PANEL, 0, bw2);
    } else {
        if (j->sel < j->top + 1) j->top = j->sel - 1;
        if (j->sel > j->top + VIS - 2) j->top = j->sel - VIS + 2;
        if (j->top > j->n - VIS) j->top = j->n - VIS;
        if (j->top < 0) j->top = 0;
        for (int k = 0; k < VIS && j->top + k < j->n; k++) {
            int i = j->top + k, ry = by + 1 + k;
            const JRow *r = &j->rows[i];
            if (r->song < 0) {
                char h[48];
                snprintf(h, sizeof h, "%s  %d/%d", SONG_GROUP_NAMES[r->group], r->got, r->total);
                term_text(t, 2, ry, h, P_GOLD, &P_PANEL);
                continue;
            }
            bool sel = i == j->sel, on = r->song == j->playing;
            RGB bg = sel ? P_PANEL_HI : P_PANEL;
            term_fill(t, 2, ry, bw2 - 5, 1, bg);
            char line[64];
            snprintf(line, sizeof line, "%s%.42s", on ? "\xE2\x99\xAA " : sel ? "\xE2\x96\xB6 " : "  ", SONG_NAMES[r->song]);
            term_text(t, 3, ry, line, on ? P_GOLD : sel ? P_WHITE : P_TEXT, &bg);
            hot_add(2, ry, bw2 - 5, 1, HOT_SONG + i);
        }
        if (j->n > VIS) {                         /* scroll position */
            int pos = (VIS - 1) * j->top / (j->n - VIS);
            for (int k = 0; k < VIS; k++) term_text(t, bw2 - 2, by + 1 + k, k == pos ? "\xE2\x96\x88" : "\xE2\x94\x82", k == pos ? P_GOLD : P_LINE, &P_PANEL);
        }
    }
    term_center(t, by + 21, j->loop_one ? "Y  LOOP ONE SONG" : "Y  AUTO-PLAY NEXT", P_PRIMARY, NULL, 0, bw2);
    hot_add(0, by + 21, bw2, 1, 1);
    static const Keycap K[5] = {{"\xE2\x96\xB2\xE2\x96\xBC", "SONG"}, {"\xE2\x97\x84\xE2\x96\xBA", "GROUP"}, {"A", "PLAY"}, {"B", "BACK"}, {"L/R", "x10"}};
    ui_keycaps_center(t, T_ROWS - 2, K, 5, NULL, 0, T_COLS);
}

static const SceneVT JB_VT = {.handle = jb_handle, .update = jb_update, .draw = jb_draw, .music = jb_music, .sound_kind = SK_JUKEBOX};

Scene *scene_jukebox(void) {
    Jukebox *j = (Jukebox *)calloc(1, sizeof(Jukebox));
    scene_base(&j->base, &JB_VT);
    j->playing = -1;
    j->loop_one = true;
    for (int g = 0; g < N_SONG_GROUPS; g++) {
        int hdr = j->n++, got = 0, total = 0;
        j->rows[hdr] = (JRow){-1, (uint8_t)g, 0, 0};
        for (int s = 0; s < N_SONGS; s++) {
            if (SONG_GROUP[s] != g) continue;
            total++;
            if (!jukebox_unlocked(s)) continue;
            got++;
            j->rows[j->n++] = (JRow){(short)s, (uint8_t)g, 0, 0};
        }
        j->rows[hdr].got = (uint8_t)(got > 255 ? 255 : got);
        j->rows[hdr].total = (uint8_t)(total > 255 ? 255 : total);
    }
    j->sel = 0;
    jb_step(j, 1);
    if (j->rows[0].song >= 0) j->sel = 0;
    return &j->base;
}
