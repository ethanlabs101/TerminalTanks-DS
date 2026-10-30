/* Battle world simulation: terrain, tanks, wind, ballistics, collision. Port of Terrain / PhysicsEngine /
 * BattleWorld (terminaltanks.py l.1198-1285, 3640-3921). No rendering here. */
#ifndef TT_WORLD_H
#define TT_WORLD_H
#include <stdbool.h>
#include "content.h"
#include "rng.h"

#define FIELD_W 128
#define FIELD_H 80
#define MAX_TANKS 6
#define TANK_W 11
#define TANK_H 6
#define PIVOT_H 4.5
#define BARREL_LEN 7.5
#define MAX_HP 3

/* PhysicsConfig defaults (l.77) */
#define PHYS_GRAVITY 32.0
#define PHYS_RANGE 1.16
#define PHYS_MIN_POWER 10.0
#define PHYS_MAX_POWER 100.0
#define PHYS_PROJ_RADIUS 0.9
#define PHYS_SUBSTEP 0.6
#define PHYS_BLAST_RADIUS 5.5
#define PHYS_SPLASH 2.5
#define PHYS_FALL 45.0

/* Animated "sentinel" colours: the original stores them as these exact RGB values (l.2238-2243). */
static const RGB COL_RAINBOW = {250, 3, 251};
static const RGB COL_ASTRAL = {251, 4, 250};
static const RGB COL_PRISMATIC = {252, 6, 247};
static const RGB COL_VOID = {247, 2, 252};
static inline bool rgb_eq(RGB a, RGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static inline bool rgb_is_live(RGB c) { return rgb_eq(c, COL_RAINBOW) || rgb_eq(c, COL_ASTRAL) || rgb_eq(c, COL_PRISMATIC) || rgb_eq(c, COL_VOID); }
static inline bool rgb_is_pixel_paint(RGB c) { return rgb_eq(c, COL_ASTRAL) || rgb_eq(c, COL_PRISMATIC) || rgb_eq(c, COL_VOID); }

enum { KIND_TANK = 0, KIND_UFO = 1 };

typedef struct {
    char name[16];
    char label[8];
    RGB tank_color, shot_color;
    bool is_ai;
    int16_t design, ammo;
    int hp;
    int team;            /* -1: own team (index) */
    int owner;           /* LAN seat that controls it */
    int8_t diff;         /* -1: the match difficulty */
    double prec;
    uint8_t kind;
    double scale, hover;
    int shield;
    bool guard;
} PlayerSetup;

typedef struct {
    int index;
    char name[16];
    char label[8];
    RGB color, shot_color;
    bool is_ai;
    double x, y;
    int facing;
    double angle, power;
    int hp, max_hp;
    RGB hull_sentinel, shot_sentinel;
    bool rgb_hull, rgb_shot;
    double hurt, recoil;
    bool destroyed;
    int16_t design, ammo;
    int team, owner;
    uint8_t kind;
    double scale, hover;
    int shield, volley, turns, charge;
    bool guard;
    double vis_dx;
    int phase;
} Tank;

static inline double tank_w(const Tank *t) { return TANK_W * t->scale; }
static inline double tank_h(const Tank *t) { return TANK_H * t->scale; }
static inline bool tank_alive(const Tank *t) { return t->hp > 0 && !t->destroyed; }
static inline double tank_rel_angle(const Tank *t) { return t->facing > 0 ? t->angle : 180.0 - t->angle; }

typedef struct {
    int width, height;
    int heights[FIELD_W];
    double scorch[FIELD_W];
    int version;
} Terrain;

enum { IMP_NONE = 0, IMP_TERRAIN, IMP_TANK, IMP_OUT };

typedef struct { double x, y; int kind; int tank; } Impact;

typedef struct {
    double x0, y0, vx, vy, g, ax, wa, wf;
} Launch;

typedef struct { Impact impact; double time; } TraceResult;

/* Scenery draws that consume the simulation rng (kept so the sequence matches the original exactly). */
typedef struct { uint8_t x, py; float ph, sp, b; } Star;
typedef struct { float x0, y0, sp, ph, amp; uint8_t ci; } Mote;
#define MAX_STARS 480
#define MAX_MOTES 128
#define MAX_STREAKS 24

typedef struct {
    int width, height;
    int map;               /* MAPDEFS index */
    PyRandom rng;          /* simulation */
    Terrain terrain;
    double max_speed;
    Tank tanks[MAX_TANKS];
    int ntanks;
    double wind, wind_max;
    bool ammo_fx;
    double clock;
    /* scenery (visual, but generated from the simulation rng) */
    int orb_x;
    double ridge_ph[6];
    Star stars[MAX_STARS];
    int nstars;
    Mote motes[MAX_MOTES];
    int nmotes;
    double streaks[MAX_STREAKS][2];
    int nstreaks;
} World;

/* terrain (l.1198) */
void terrain_init(Terrain *t, int w, int h, const MapDef *m, PyRandom *rng);
int  terrain_height_at(const Terrain *t, double x);
int  terrain_support(const Terrain *t, double x);
void terrain_flatten(Terrain *t, double cx, int half);
void terrain_carve(Terrain *t, double cx, double cy, double r, int *x0, int *x1);
void terrain_mound(Terrain *t, double cx, double r, double height, int *x0, int *x1);
void terrain_trench(Terrain *t, double cx, double half, double depth, int *x0, int *x1);
void terrain_smooth(Terrain *t, double cx, double r, int *x0, int *x1);
double sample_profile(const double *pts, int n, double u, int smooth);

/* world */
void world_scenery_init(World *w);   /* the Scenery constructor's rng draws */
void world_init(World *w, int width, int height, int map, uint32_t seed, const PlayerSetup *players, int n, double wind_setting);
void world_roll_wind(World *w);
void world_drift_wind(World *w);
int  world_collision(const World *w, double x, double y, int skip, int *tank);
bool world_inside_tank(const World *w, int idx, double x, double y);
double world_distance_to_tank(const Tank *t, double x, double y);
bool world_settle(World *w, double dt);
void world_terrain_op(World *w, int mode, double x, double y, double r);

/* ballistics */
double phys_speed_for(const World *w, double power);
Launch phys_make_launch(const World *w, double ox, double oy, double angle, double power, double gravity, double wind);
void launch_at(const Launch *L, double t, double *x, double *y);
void tank_muzzle(const Tank *t, double angle, double *x, double *y);
void muzzle_point(double x, double y, double angle, int design, double scale, double recoil, double *mx, double *my);

/* probes: owner probe semantics (l.3828). left = state flag owned by the caller. */
typedef struct { const World *w; int owner; bool left; } OwnerProbe;
int owner_probe(OwnerProbe *p, double x, double y, int *tank);

typedef int (*ProbeFn)(void *ctx, double x, double y, int *tank);
TraceResult phys_trace(const Launch *L, ProbeFn probe, void *ctx, double dt, double max_time);
/* trace that also records dotted guide points every `spacing` px (aim guide) */
int phys_trace_points(const Launch *L, ProbeFn probe, void *ctx, double dt, double spacing, double max_time,
                      float *pts, int max_pts);

#endif
