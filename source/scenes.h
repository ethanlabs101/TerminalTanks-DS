/* Scene constructors. Each mirrors one Scene class of terminaltanks.py; see docs/FEATURE_PARITY.md. */
#ifndef TT_SCENES_H
#define TT_SCENES_H
#include "app.h"
#include "../core/session.h"
#include "../core/battle.h"

/* The one live match (the original passes its MatchSession object from scene to scene). */
extern Session g_sess;
extern Rules g_rules_last;          /* rules of the match being set up */

Scene *scene_boot(void);
Scene *scene_title(void);
Scene *scene_menu(void);
Scene *scene_notice(const char *title, const char *text);
Scene *scene_howto(void);
Scene *scene_settings(void);
Scene *scene_jukebox(void);
Scene *scene_links(void);
Scene *scene_cheats(void);
Scene *scene_profiles(void);

/* match flow */
enum { RK_SINGLE = 0, RK_TWO = 1, RK_LAN = 2 };
Scene *scene_rules(int kind);
enum { SM_SINGLE = 0, SM_TWO, SM_TOURNAMENT, SM_LAN, SM_MISSION };
typedef void (*SetupDone)(const Loadout *los, int n);
Scene *scene_setup(int mode, int level, const Rules *rules, SetupDone done, int pages);
Scene *scene_mapselect(void);
Scene *scene_rps(void);
Scene *scene_battle(void);
Scene *scene_pause(Scene *battle);
Scene *scene_confirm_leave(void);
Scene *scene_round_result(Scene *battle);
Scene *scene_round_map(int picker);
Scene *scene_round_map_wait(int picker);
Scene *scene_match_result(void);
Scene *scene_practice(int lesson, int adv);
Scene *scene_tutorial(bool adv);

/* progression */
Scene *scene_tournament(int level);
Scene *scene_tournament_result(void);
Scene *scene_missions(int number);
Scene *scene_coop_choice(int mission);
Scene *scene_mission_result(void);
Scene *scene_shop(int page);
Scene *scene_workshop(void);
Scene *scene_studio(void);
Scene *scene_ticket_result(void);
Scene *scene_martyr_rewards(void);
Scene *scene_martyr_finale(void);
Scene *scene_martyr_outro(void);
void reward_label(int uid, char *lab, int nl, char *name, int nn);
typedef Scene *(*NextFn)(void);
Scene *scene_cinematic(int which, NextFn next);
void whisper(int story, NextFn next);       /* the story popups: shows once, then goes to next() */

/* LAN over DS wireless */
Scene *scene_lan_menu(void);
Scene *scene_lan_host(void);
Scene *scene_lan_join(void);

/* starting matches (Application.start_match / start_tournament / start_ticket / start_martyr) */
void start_match_session(bool rps);         /* g_sess already built */
void start_tournament(int level, const PlayerSetup *me);
void start_ticket(int ticket);
void start_martyr(bool intro, bool replay);
void build_mission(int mission, const Loadout *los, int nlo, bool wingman, const Loadout *guest, bool net);

/* session routing (MatchSession.finish_scene / exit_scene / restart / exit_label) */
Scene *session_finish_scene(void);
Scene *session_exit_scene(void);
void session_restart(void);
const char *session_exit_label(void);

/* helpers shared by scenes */
void backdrop_scene_update(Scene *s, float dt);
void backdrop_scene_draw(Scene *s, Term *t);
float backdrop_frame_time(void);   /* g_app.time of the last backdrop redraw */
const char *unlock_hint(bool tank, int idx, char *buf, int n);
int  unlocked_map_list(int16_t *out);        /* MAPS + unlocked special maps (MapSelect order) */

#endif
