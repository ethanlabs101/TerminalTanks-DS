/* Shot planning: the whole shot (arcs, bounces, splits, bomblets, chains, fuses) is decided at fire time on the fixed
 * 20 ms physics grid. Port of make_ammo_launch / plan_shot (terminaltanks.py l.8337-8444). */
#ifndef TT_SHOT_H
#define TT_SHOT_H
#include "world.h"

#define SPECIAL_DMG 3
#define SPECIAL_RADIUS 2.0
#define MAX_FLIGHTS 16
#define MAX_EVENTS_PER_FLIGHT 20

typedef struct {
    double x, y;
    int kind;            /* IMP_* */
    int tank;            /* -1 */
    double radius;
    int dmg;
    int mode;            /* TERR_BLAST/MOUND/TRENCH/GLASS or TERR_FX */
    double delay;
    bool first;
    double pull;
    bool multi;
    bool big;
} Ev;

typedef struct {
    Launch launch;
    TraceResult trace;
    double delay;
    Ev events[MAX_EVENTS_PER_FLIGHT];
    int nevents;
} FlightSpec;

typedef struct {
    FlightSpec f[MAX_FLIGHTS];
    int n;
} ShotPlan;

/* the ammo whose mechanics apply (NULL = plain shell) */
const AmmoType *shot_mechanics(const World *w, const Tank *t, bool special);
Launch make_ammo_launch(const World *w, const Tank *t, double angle, double power, const double *wind, bool special);
void plan_shot(const World *w, const Tank *tank, bool special, ShotPlan *out);

#endif
