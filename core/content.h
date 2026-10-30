/* Content tables (generated from the Python reference by tools/gen_content.py) and their types. */
#ifndef TT_CONTENT_H
#define TT_CONTENT_H

#include <stdint.h>
#include <stddef.h>

typedef struct { uint8_t r, g, b; } RGB;

typedef struct {
    uint8_t n;            /* number of barrel lanes */
    double offsets[4];
    double length;
    uint8_t style;        /* 0 plain 1 rail 2 coil 3 prism 4 crystal 5 flame 6 spin 7 void 8 bolt 9 rune */
} BarrelSpec;

typedef struct {
    const char *key, *name, *blurb;
    const char *mask;     /* 66 chars: 6 rows x 11 columns, row 0 = top */
    uint8_t barrel, glow;
    uint32_t ambient;     /* bitmask of AMB_* */
    RGB accent;
    uint8_t has_paint;
    RGB paint;
    int8_t turret, hull, track;   /* -1: hand-drawn mask */
    double hover;
} TankDesign;

/* mechanic presence flags (the Python code uses different defaults per call site, so presence matters) */
enum {
    MF_G = 1 << 0, MF_WIND = 1 << 1, MF_SPEED = 1 << 2, MF_RADIUS = 1 << 3, MF_BR = 1 << 4, MF_SUBR = 1 << 5,
    MF_CSPACE = 1 << 6, MF_CR = 1 << 7, MF_CSTEP = 1 << 8, MF_FUSE = 1 << 9, MF_FR = 1 << 10, MF_PULL = 1 << 11,
    MF_BOUNCE = 1 << 12, MF_CLUSTER = 1 << 13, MF_CHAIN = 1 << 14, MF_DMG = 1 << 15, MF_MULTI = 1 << 16,
    MF_WOB = 1 << 17, MF_TERRAIN = 1 << 18, MF_APEX = 1 << 19
};
enum { TERR_BLAST = 0, TERR_MOUND, TERR_TRENCH, TERR_GLASS, TERR_FX };
enum { APEX_NONE = 0, APEX_SPLIT, APEX_JUMP };

typedef struct {
    const char *key, *name, *blurb;
    uint8_t proj, trail, boom;
    uint32_t mf;          /* MF_* presence flags: mf == 0 means a plain (cosmetic) shell */
    double g, wind, speed, radius, br, subr, cspace, cr, cstep, fuse, fr, pull, wob_a, wob_f, apex_v;
    uint8_t bounce, cluster, chain, dmg, multi, terrain, apex_kind, apex_n;
} AmmoType;

typedef struct { const char *name; RGB color; } ElemInfo;

typedef struct {
    RGB sky_top, sky_bottom, far, orb, surface, topsoil, soil, deep;
    double star_density;
    uint8_t aurora, fx, buildings;
} MapTheme;

typedef struct {
    const char *key, *name, *desc;
    const double *pts;    /* npts (x, h) pairs */
    uint8_t npts, smooth;
    double roughness;
    double spawns[2];
    uint16_t theme;
    uint8_t cover, relief, special;
    RGB accent;
} MapDef;

typedef struct { const char *name; RGB c; } NamedColor;

typedef struct { double t; RGB c; } SkyStop;
typedef struct {
    const char *key, *name, *blurb;
    uint8_t nsky;
    SkyStop sky[4];
    RGB far, ground, ground_top, accent, logo_a, logo_b;
    uint8_t fx;
    int16_t hero, hero_ammo;
    uint8_t stars, aurora, corona, nlayers;
    uint8_t layers[4];
} HomeTheme;

enum { DIFF_EASY = 0, DIFF_NORMAL, DIFF_HARD, DIFF_MASTER };
enum { RULE_STANDARD = 0, RULE_STREAK, RULE_FLAWLESS, RULE_ONESTRIKE, RULE_GLASS, RULE_GALE };

typedef struct {
    uint8_t number;
    const char *name, *boss;
    uint8_t difficulty, wins;
    int16_t tank, ammo;
    uint8_t boss_color, boss_shot;
    const char *blurb;
    int16_t map;          /* index into MAPDEFS or -1 */
    double precision;
    uint8_t rule, extra;
} TournLevel;

enum { MK_MARATHON = 0, MK_2V1, MK_UFO, MK_COOP, MK_RUSH, MK_MOTHER };
typedef struct {
    uint8_t number;
    const char *name;
    uint8_t kind, diff, diff_end, stages, target, fail_at, hp, enemies, enemy_hp, ufo_hp;
    double prec;
    const unsigned char *levels;
    uint8_t nlevels, coop_ufo;
    int16_t map;
    const char *blurb;
    int16_t rewards[4];
    uint8_t nrewards;
} TTMission;

typedef struct {
    const char *key, *name;
    int16_t tank, ammo, map, theme;
    uint8_t hp;
    double prec;
    const char *taunt;
} TicketBoss;

typedef struct {
    int16_t uid;
    uint8_t page, tier;
    const char *name, *blurb;
    int32_t coins, credits;
} ShopItem;

typedef struct {
    const char *code;
    int16_t grants[2];
    uint8_t ngrants, everything;
    int16_t activate_theme;
    const char *text;
    uint8_t level;        /* unlocklevelN codes: N */
} Cheat;

typedef struct {
    double angle_sigma, power_sigma, gravity_error, sim_dt;
    int terrain_aware;
    double learn_rate, think_time;
    int angle_step, pick_from, refine;
    double wind_error;
    int adaptive;
} AIProfile;

typedef struct { int on; double density, speed; RGB pal[3]; double alpha; } MoteStyle;

#include "gen/content_enums.h"

extern const BarrelSpec BARRELS[];
extern const TankDesign DESIGNS[];
extern const AmmoType AMMOS[];
extern const short MAGE_AMMO_IDX[6][3];
extern const ElemInfo ELEMENTS[6];
extern const unsigned char PHASE_ELEMS[3][7];
extern const MapTheme THEMES[];
extern const MapDef MAPDEFS[];
extern const NamedColor COLOR_CHOICES[];
extern const char *const HOME_ORDER[];
extern const HomeTheme HOME_THEMES[];
extern const TournLevel TOURNAMENT[];
extern const TTMission MISSIONS[];
extern const TicketBoss TICKETS[];
extern const char *const UNLOCK_UIDS[];
extern const char *const UNLOCK_LABELS[];
extern const ShopItem SHOP[];
extern const short LEVEL_REWARDS[][6];
extern const short TICKET_REWARDS[][4];
extern const short MARTYR_REWARDS[];
extern const Cheat CHEATS[];
extern const AIProfile AI_PROFILES[4];
extern const MoteStyle MOTE_STYLES[];

/* lookups (content.c) */
int design_by_key(const char *key);
int ammo_by_key(const char *key);
int map_by_key(const char *key);
int uid_index(const char *uid);
int color_index_by_name(const char *name);

#endif
