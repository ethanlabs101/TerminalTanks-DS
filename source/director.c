/* The music director and sound glue (see director.h). Follows tanks/app.py (sfx, _direct_music, _ui_sound, the wallet
 * chimes in step) and the sfx calls of tanks/scenes/battle.py, which the battle core reports as FX_SFX requests. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "director.h"
#include "scenes.h"
#include "../core/content.h"
#include "../core/session.h"

static float g_last_t[N_SFX];
static int g_last_battle = -1;
static int g_coins_seen = -1, g_granted_seen = -1;
static int g_popup_song = -1;
static float g_popup_t;                            /* > 0: the NOW PLAYING card is showing (seconds) */

/* NOW PLAYING: slides in at the top right of the top screen, stays 3 s, slides out */
void director_draw_popup(Term *t) {
    if (g_popup_t <= 0 || g_popup_song < 0) return;
    float a = g_popup_t < 0.3f ? g_popup_t / 0.3f : g_popup_t > 3.7f ? (4.0f - g_popup_t) / 0.3f : 1.0f;
    const char *nm = SONG_NAMES[g_popup_song];
    int w = (int)strlen(nm) + 6;
    if (w < 18) w = 18;
    if (w > T_COLS - 2) w = T_COLS - 2;
    int x = T_COLS - (int)(w * a + 0.5f);
    if (x >= T_COLS) return;
    RGB bg = {14, 22, 34}, line = {94, 230, 208};
    term_box(t, x, 1, w, 4, TB_SINGLE, line, bg, "", line);
    term_text(t, x + 2, 2, "\xE2\x99\xAA NOW PLAYING", (RGB){255, 190, 80}, &bg);
    char buf[64];
    snprintf(buf, sizeof buf, "%.*s", w - 4, nm);
    term_text(t, x + 2, 3, buf, P_WHITE, &bg);
}

/* JukeboxScene's shelf (tanks/audio/director.py jukebox_unlocked): what you heard, plus what progress has reached */
bool jukebox_unlocked(int song) {
    const SaveData *sv = &g_app.save;
    if (song < 0 || song >= N_SONGS) return false;
    if (sv->heard[song >> 3] & (1 << (song & 7))) return true;
    if (song == SONG_MAIN_THEME || song == SONG_TUTORIAL || song == SONG_SHOP_JINGLE || song == SONG_WORKSHOP) return true;
    for (int i = 0; i < 20; i++) if (TOURN_SONG[i] == song) return (sv->completed >> i) & 1;
    for (int i = 0; i < N_TICKETS; i++) if (TICKET_SONG[i] == song) return (sv->tickets_won >> i) & 1;
    if (song == SONG_WHISPER) return sv->tickets_won || sv->martyr_won;
    for (int i = 0; i < 3; i++)
        if (MARTYR_SONG[i] == song) return sv->martyr_won || (sv->martyr_seen && i <= sv->martyr_phase);
    if (song == SONG_TIME_MACHINE || song == SONG_THE_BLESSING || song == SONG_PROTECTOR) return sv->martyr_won;
    if (song == SONG_SAUCER_ATTACK || song == SONG_SQUAD_UP || song == SONG_FIRST_CONTACT) return save_missions_open(sv);
    if (song == SONG_MOTHERSHIP || song == SONG_BOSS_RUSH)
        return ((sv->mission_completed >> (N_MISSIONS - 1)) & 1) || (song == SONG_BOSS_RUSH && ((sv->mission_completed >> 39) & 1));
    for (int i = 0; i < N_HOME; i++) if (HOME_SONG[i] == song && save_home_unlocked(sv, i)) return true;
    for (int i = 0; i < N_MAPS_ALL; i++) if (MAP_SONG[i] == song && save_map_unlocked(sv, i)) return true;
    return false;
}

void sfx_play(int id, float vol, float pan, float rate, float gap) {
    if (id < 0 || id >= N_SFX) return;
    float now = (float)g_app.time;
    if (gap > 0 && g_last_t[id] != 0 && now - g_last_t[id] < gap) return;
    g_last_t[id] = now;
    audio_sfx(id, vol, pan, rate);
}

int last_battle_song(void) { return g_last_battle; }
int music_result(Scene *s, int *mood) { (void)s; *mood = MOOD_RESULT; return g_last_battle; }
int music_shop(Scene *s, int *mood) { (void)s; *mood = MOOD_MENU; return SONG_SHOP_JINGLE; }
int music_workshop(Scene *s, int *mood) { (void)s; *mood = MOOD_MENU; return SONG_WORKSHOP; }
int music_blessing(Scene *s, int *mood) { (void)s; *mood = MOOD_STORY; return SONG_THE_BLESSING; }

/* MatchSession.song() and its overrides (tanks/match/sessions.py, martyr.py) */
int session_song(void) {
    const Session *s = &g_sess;
    switch (s->type) {
    case SESS_TOURNAMENT: {
        int lv = s->level < 1 ? 1 : s->level > 20 ? 20 : s->level;
        return TOURN_SONG[lv - 1];
    }
    case SESS_MISSION: {
        if (s->mission >= 1 && s->mission <= N_MISSIONS) {
            const TTMission *m = &MISSIONS[s->mission - 1];
            if (m->kind == MK_COOP && m->coop_ufo) return SONG_SAUCER_ATTACK;
            if (m->kind == MK_UFO) return SONG_SAUCER_ATTACK;
            if (m->kind == MK_MOTHER) return SONG_MOTHERSHIP;
            if (m->kind == MK_RUSH) return SONG_BOSS_RUSH;
            if (m->kind == MK_COOP || m->kind == MK_2V1) return SONG_SQUAD_UP;
        }
        break;
    }
    case SESS_TICKET:
        if (s->ticket >= 0 && s->ticket < N_TICKETS) return TICKET_SONG[s->ticket];
        break;
    case SESS_MARTYR:
        return MARTYR_SONG[s->phase_i < 0 ? 0 : s->phase_i > 2 ? 2 : s->phase_i];
    case SESS_PRACTICE:
        return SONG_TUTORIAL;
    }
    if (s->current_map >= 0 && s->current_map < N_MAPS_ALL) return MAP_SONG[s->current_map];
    return SONG_STEEL_RAIN;
}

/* story.cinematic_song: the ending, a Martyr phase's theme before that fight, his time machine, or the voice */
int cinematic_song(const char *const *who, const int *stage, int n) {
    for (int i = 0; i < n; i++) if (stage[i] == CS_WIN) return SONG_THE_BLESSING;
    for (int i = 0; i < n; i++) if (stage[i] >= CS_MAP0 && stage[i] <= CS_MAP2) return MARTYR_SONG[stage[i] - CS_MAP0];
    for (int i = 0; i < n; i++) if (who[i] && !strcmp(who[i], "THE MARTYR")) return SONG_TIME_MACHINE;
    return SONG_WHISPER;
}

/* Application._ui_sound */
void director_ui_sound(Scene *top, const IEv *e) {
    if (!top) return;
    if (top->vt->sound_kind == SK_SILENT) return;
    if (top->vt->sound_kind == SK_BATTLE) {
        if (!battle_scene_local_aiming(top)) return;
        if (e->action == A_LEFT || e->action == A_RIGHT) sfx_play(SFX_AIM_TICK, 0.6f, 0, 1, 0.035f);
        else if (e->action == A_UP || e->action == A_DOWN) sfx_play(SFX_POWER_TICK, 0.6f, 0, 1, 0.035f);
        return;
    }
    switch (e->action) {
    case A_UP: case A_DOWN: sfx_play(SFX_UI_MOVE, 0.7f, 0, 1, 0.03f); break;
    case A_LEFT: case A_RIGHT: sfx_play(SFX_UI_TAB, 0.7f, 0, 1, 0.03f); break;
    case A_CONFIRM: case A_FIRE: case A_TOUCH: sfx_play(SFX_UI_SELECT, 0.8f, 0, 1, 0.05f); break;
    case A_BACK: sfx_play(SFX_UI_BACK, 0.8f, 0, 1, 0.05f); break;
    case A_OTHER:
        if (e->raw > 0 && e->raw < 128 && ((e->raw >= '0' && e->raw <= '9') || ((e->raw | 32) >= 'a' && (e->raw | 32) <= 'z')))
            sfx_play(SFX_UI_TYPE, 0.5f, 0, 1, 0.02f);
        break;
    }
}

/* the sfx calls of BattleScene, as reported by the battle core */
void director_battle_fx(const FxReq *r, double width) {
    float pan = (float)((r->x / (width > 1 ? width : 1) - 0.5) * 1.2);
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    float v = (float)r->p, rate = (float)r->q;
    switch (r->a) {
    case BS_TURN_YOU: sfx_play(SFX_TURN_YOU, v, 0, 1, 0); break;
    case BS_TURN_CPU: sfx_play(SFX_TURN_CPU, v, 0, 1, 0); break;
    case BS_WIND: sfx_play(SFX_WIND_GUST, v, pan, 1, 4.0f); break;
    case BS_UFO_HOP: sfx_play(SFX_UFO_HOP, v, pan, 1, 0.2f); break;
    case BS_UFO_VOLLEY: sfx_play(SFX_UFO_VOLLEY, v, pan, 1, 0); break;
    case BS_FIRE: sfx_play(SFX_FIRE_FIRST + r->n, v, pan, 1, 0); break;
    case BS_BLESS_CHOIR: sfx_play(SFX_BLESS_CHOIR, v, pan, 1, 0); break;
    case BS_SPECIAL_FIRE: sfx_play(SFX_SPECIAL_FIRE, v, pan, 1, 0); break;
    case BS_BLESS_STAR: sfx_play(SFX_BLESS_STAR, v, pan, 1, 0); break;
    case BS_BLESS_TICK: sfx_play(SFX_BLESS_TICK, v, 0, rate, 0); break;
    case BS_BLESS_PILLAR: sfx_play(SFX_BLESS_PILLAR, v, pan, 1, 0); break;
    case BS_BLESS_PILLAR_HIT: sfx_play(SFX_BLESS_PILLAR, v, pan, rate, 0.03f); break;
    case BS_BLESS_NUKE: sfx_play(SFX_BLESS_NUKE, v, pan, 1, 0); break;
    case BS_BLESS_FIREWALL: sfx_play(SFX_BLESS_FIREWALL, v, 0, 1, 0); break;
    case BS_HIT: sfx_play(SFX_HIT_FIRST + r->n, v, pan, rate, 0.04f); break;
    case BS_MOUND: sfx_play(SFX_MOUND, v, pan, 1, 0.1f); break;
    case BS_SHIELD: sfx_play(SFX_SHIELD_BLOCK, v, pan, 1, 0.05f); break;
    case BS_TANK_HIT: sfx_play(SFX_TANK_HIT, v, pan, 1, 0.05f); break;
    case BS_DESTROY: sfx_play(SFX_TANK_DESTROY, v, pan, 1, 0); break;
    case BS_DEBRIS: sfx_play(SFX_DEBRIS, v, pan, 1, 0); break;
    case BS_SPECIAL_CHARGE: sfx_play(SFX_SPECIAL_CHARGE, v, pan, 1, 0); break;
    }
}

static int granted_count(void) {
    int n = 0;
    for (size_t i = 0; i < sizeof g_app.save.granted; i++) n += __builtin_popcount(g_app.save.granted[i]);
    return n;
}

/* Application._direct_music + the wallet / unlock chimes, once per frame */
void director_frame(float dt) {
    int mode, mv, sv;
    settings_audio(&g_app.settings, &mode, &mv, &sv);
    audio_settings(mode == SND_ON ? AUDIO_ON : mode == SND_BEEP ? AUDIO_BEEP : AUDIO_OFF, mv, sv);
    int song = -1, mood = MOOD_MENU;
    for (int i = g_app.nstack - 1; i >= 0 && song < 0; i--) {
        Scene *s = g_app.stack[i];
        if (s->vt->music) {
            mood = MOOD_MENU;
            song = s->vt->music(s, &mood);
        }
    }
    if (song < 0 || song >= N_SONGS) {
        song = HOME_SONG[app_home_theme()];
        mood = MOOD_MENU;
    }
    if (mood == MOOD_BATTLE) {
        g_last_battle = song;
        mood = MOOD_MENU;
    }
    audio_music(song, 1.5f);
    audio_mood(mood);
    if (song != g_popup_song) {                    /* a new song: remember it was heard, show the NOW PLAYING card */
        g_popup_song = song;
        if (!(g_app.save.heard[song >> 3] & (1 << (song & 7)))) {
            g_app.save.heard[song >> 3] |= (uint8_t)(1 << (song & 7));
            g_app.save_dirty = true;
        }
        Scene *top = g_app.nstack ? g_app.stack[g_app.nstack - 1] : NULL;
        int mode2, mv2, sv2;
        settings_audio(&g_app.settings, &mode2, &mv2, &sv2);
        if (!g_app.save.song_popup_off && mode2 == SND_ON && audio_ready() && !(top && top->vt->sound_kind == SK_JUKEBOX)) g_popup_t = 0.001f;
    }
    if (g_popup_t > 0) {
        g_popup_t += dt;
        if (g_popup_t > 4.0f) g_popup_t = 0;
    }
    if (g_app.nstack) {
        int coins = g_app.save.coins, got = granted_count();
        if (g_coins_seen >= 0) {
            if (coins < g_coins_seen) sfx_play(SFX_UI_BUY, 0.9f, 0, 1, 0.3f);
            else if (coins > g_coins_seen) sfx_play(SFX_PAYOUT, 0.8f, 0, 1, 0.6f);
            if (got > g_granted_seen) sfx_play(SFX_UI_UNLOCK, 0.9f, 0, 1, 0.8f);
        }
        g_coins_seen = coins;
        g_granted_seen = got;
    }
    audio_update(dt);
}
