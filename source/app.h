/* Application: scene stack, transitions, input events, wallet, backdrops, profiles (port of Application, Scene,
 * InputManager, Transition; terminaltanks.py l.483-530, 7509-7536, 13213-13410). */
#ifndef TT_APP_H
#define TT_APP_H
#include "term.h"
#include "platform.h"
#include "../core/save.h"
#include "../core/rng.h"

/* ---- input events (Action enum, l.483). DS: A=CONFIRM, B=BACK, START=PAUSE, d-pad, L/R held = coarse ("Shift"). */
enum { A_NONE = 0, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_CONFIRM, A_FIRE, A_BACK, A_PAUSE, A_OTHER, A_TOUCH };
/* raw keys for A_OTHER (and attached to the d-pad/buttons too) */
enum { K_NONE = 0, K_X = 0x10000, K_Y, K_SELECT, K_L, K_R, K_BKSP, K_DEL, K_ENTER, K_SPACE };
typedef struct {
    int action;
    bool coarse;
    int raw;           /* K_* or a printable ASCII char (on-screen keyboard), or the hotspot id for A_TOUCH */
    int tx, ty;        /* touch position in tall-terminal cells (A_TOUCH) */
} IEv;
static inline bool ev_confirm(const IEv *e) { return e->action == A_CONFIRM || e->action == A_FIRE; }

/* ---- scenes */
typedef struct Scene Scene;
typedef struct {
    void (*enter)(Scene *);
    void (*exit)(Scene *);
    void (*handle)(Scene *, const IEv *);
    void (*update)(Scene *, float dt);
    void (*update_ambient)(Scene *, float dt);
    void (*draw)(Scene *, Term *);
    void (*draw_px)(Scene *, Surf *top, Surf *bot);   /* optional: pixel layer drawn UNDER transparent cells */
    void (*draw_over)(Scene *, Surf *top, Surf *bot); /* optional: pixel overlay drawn over the cells */
    void (*destroy)(Scene *);
} SceneVT;
struct Scene {
    const SceneVT *vt;
    bool overlay, ambient, show_wallet, home;
};
void scene_base(Scene *s, const SceneVT *vt);

/* ---- touch hotspots: scenes register them while drawing (tall-terminal cell coords, rows 24..47 = touch screen) */
void hot_add(int x, int y, int w, int h, int id);

/* ---- backdrops (menus) */
typedef struct Backdrop Backdrop;
void backdrop_update(Backdrop *b, float dt);
void backdrop_draw(Backdrop *b, Term *t, float time);
void backdrop_celebrate(Backdrop *b, const RGB *colors, int n);

/* ---- profiles (ProfileBook l.10093) */
#define PROFILE_MAX 3     /* l.10075 */
typedef struct { char id[12]; char name[16]; } ProfileInfo;
typedef struct {
    ProfileInfo p[PROFILE_MAX];
    int n;
    int active;          /* index */
} ProfileBook;

typedef struct {
    SaveData save;
    Settings settings;
    PyRandom rng;
    double time;
    bool running;
    Scene *stack[8];
    int nstack;
    Scene *tr_scene;
    int tr_phase;        /* 0 none, 1 out, 2 in */
    float tr_t;
    float wallet_flash;
    float pt_flush;
    ProfileBook profiles;
    Backdrop *backdrop;  /* ambient */
    bool save_dirty;
} App;
extern App g_app;

void app_init(void);
void app_step(float dt);
void app_goto(Scene *s);
void app_goto_now(Scene *s);
void app_push(Scene *s);
void app_pop(void);
Scene *app_top(void);
Backdrop *app_home_backdrop(void);
int  app_home_theme(void);            /* index into HOME_ORDER (falls back to CLASSIC) */
void app_apply_settings(void);        /* capture settings into the save and write it */
void app_flush(void);                 /* write the save file */
void app_switch_profile(int idx);
void app_delete_profile(int idx);
int  app_new_profile(const char *name);
void app_rename_profile(int idx, const char *name);
void app_feedback(int kind);
bool app_load_profile_save(int idx, SaveData *out);
extern Input g_input;
Backdrop *backdrop_new_ambient(void);          /* sound: 0 explosion, 1 destroy */

#endif
