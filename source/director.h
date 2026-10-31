/* The music director and sound glue (Application._direct_music / sfx / _ui_sound, terminaltanks 2.0.1 tanks/app.py):
 * which song plays for what is on screen, menu clicks, wallet chimes, and the battle's sound events. */
#ifndef TT_DIRECTOR_H
#define TT_DIRECTOR_H
#include "app.h"
#include "audio.h"
#include "../core/battle.h"

#define MOOD_BATTLE 3                     /* like MOOD_MENU, and remembered for the result screens */
void sfx_play(int id, float vol, float pan, float rate, float gap);
int  session_song(void);                  /* MatchSession.song() for g_sess */
int  last_battle_song(void);              /* for the result screens (-1 = none yet) */
int  cinematic_song(const char *const *who, const int *stage, int n);
void director_ui_sound(Scene *top, const IEv *e);
void director_battle_fx(const FxReq *r, double world_width);
void director_frame(float dt);
void director_draw_popup(Term *t);       /* the NOW PLAYING card, over everything */
bool jukebox_unlocked(int song);
bool battle_scene_local_aiming(Scene *s); /* scn_battle.c */
/* SceneVT.music for the scenes that pick their own song */
int music_result(Scene *s, int *mood);    /* results: the last battle's song, held back */
int music_shop(Scene *s, int *mood);
int music_workshop(Scene *s, int *mood);
int music_blessing(Scene *s, int *mood);  /* the Martyr's ending */
#endif
