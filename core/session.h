/* Match sessions: MatchSession and its Tournament / Mission / Ticket / Martyr / Practice variants
 * (terminaltanks.py l.5427-5998, 9618, 12620). Scoring rules, stage opponents, map rotation, payouts. */
#ifndef TT_SESSION_H
#define TT_SESSION_H
#include <math.h>
#include "world.h"
#include "save.h"

enum { SESS_MATCH = 0, SESS_TOURNAMENT, SESS_MISSION, SESS_TICKET, SESS_MARTYR, SESS_PRACTICE };
enum { MODE_SINGLE = 0, MODE_TWO, MODE_LAN, MODE_TOURNAMENT, MODE_MISSION, MODE_TICKET, MODE_MARTYR };

#define MAX_PLAYED 128

typedef struct {
    int type;
    /* MatchConfig */
    PlayerSetup players[MAX_TANKS];
    int nplayers;
    bool single;
    int difficulty;
    int mode;
    double ai_precision;
    int target;
    char label[24];
    bool net;             /* played over local wireless */
    int my_owner;         /* this console's seat (0 host, 1 client) */
    int special_wait;
    bool pick_maps, ammo_fx, special, blessing;
    /* MatchSession */
    PyRandom rng;
    int wins[2];
    int round_no;
    int16_t played[MAX_PLAYED];
    int nplayed;
    int current_map;
    int pending_map;      /* -1 none (random) */
    int first_offset;
    bool has_wind_override;
    double wind_override;
    bool paid, committed, first_clear, has_payout;
    int payout_coins, payout_credits;
    /* tournament */
    int level;            /* 1..20 */
    /* mission */
    int mission;          /* 1..50 */
    int stage;
    bool stage_cleared;
    PlayerSetup humans[2];
    int nhumans;
    /* ticket */
    int ticket;
    /* martyr */
    int phase_i;
    bool won_phase, replay;
    /* practice */
    int lesson, adv;
} Session;

/* builders */
void player_setup_init(PlayerSetup *p, const char *name, const char *label, RGB tank, RGB shot, bool ai, int design, int ammo, int hp);
void session_init_match(Session *s, const PlayerSetup *players, int n, int mode, int difficulty, int target, const char *label,
                        bool ammo_fx, bool special, bool pick_maps, bool blessing, int first_map, uint32_t seed);
void session_init_tournament(Session *s, int level, const PlayerSetup *me, uint32_t seed);
void session_init_mission(Session *s, int mission, const PlayerSetup *humans, int nhumans, uint32_t seed);
void session_init_ticket(Session *s, int ticket, const Loadout *lo, uint32_t seed);
void session_init_martyr(Session *s, const Loadout *lo, int phase, bool replay, uint32_t seed);
void session_init_practice(Session *s, const PlayerSetup *me, const PlayerSetup *target, uint32_t seed, int lesson, int adv, double wind);

PlayerSetup pick_ai_kit(const SaveData *save, PyRandom *rng, int difficulty, int player_color, const int16_t *ammo_pool, int npool);
PlayerSetup boss_setup(int level, int player_color);
int  humans_for(int mission, const Loadout *lo, int nlo, bool wingman, const Loadout *guest, PlayerSetup *out);

/* rules */
int  session_starting_player(const Session *s);
uint32_t session_round_seed(Session *s);
void session_record_round(Session *s, int winner);
bool session_over(const Session *s);
bool session_completed(const Session *s);
int  session_champion(const Session *s);
void session_advance(Session *s);
void session_commit(Session *s, SaveData *save);       /* payouts & progression: once, when it matters */
int  session_dmg_mult(const Session *s, const Tank *shooter);
bool session_wants_interlude(const Session *s);        /* Martyr: a cinematic between phases */
int  session_stage_diff(const Session *s);

/* presentation strings (original wording) */
void session_hud_wins(const Session *s, int team, char *out, int n);
void session_hud_title(const Session *s, char *out, int n);
void session_banner_title(const Session *s, char *out, int n);
void session_banner_note(const Session *s, char *out, int n);
typedef struct { char title[48], headline[64], score[64], sub[64], next[80]; } RoundSummary;
void session_round_summary(const Session *s, int winner, RoundSummary *rs);
void session_team_name(const Session *s, int team, char *out, int n);
int  session_team_player(const Session *s, int team);  /* first player index of a team */

/* level / mission texts */
int  level_fail_at(int level);           /* -1 = never fails */
const char *level_rule_tag(int level);
const char *level_rule_text(int level);
void level_objective(int level, char *out, int n);
const char *mission_type_name(int m);
void mission_objective(int m, char *out, int n);
void mission_rule_text(int m, char *out, int n);

/* payouts */
void tournament_payout(int level, bool won, bool first, int *coins, int *credits);
int  mission_first_coins(int m);
int  mission_repeat_coins(int m);

#endif
