/* Progress, unlocks, wallet, loadouts and settings. Port of SaveData (terminaltanks.py l.4940-5336), the cheat
 * codes (l.5342-5400), the workshop rules (l.4806-4887) and Settings (l.752). Platform-independent: it serialises to a
 * byte buffer; the DS layer writes that buffer to the SD card. */
#ifndef TT_SAVE_H
#define TT_SAVE_H
#include <stdbool.h>
#include <stdint.h>
#include "content.h"
#include "rng.h"

#define CUSTOM_TANK_MAX 24
#define CUSTOM_TANK_FEE 2500
#define HOVER_COINS 25000
#define HOVER_CREDITS 50
#define CUSTOM_REMOTE_SLOTS 4               /* LAN: the peer's custom tank, usable for that match only */
#define DESIGN_CUSTOM_FIRST N_DESIGNS
#define DESIGN_REMOTE_FIRST (N_DESIGNS + CUSTOM_TANK_MAX)
#define N_DESIGNS_RT (N_DESIGNS + CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS)

/* paints: 24 colours then the four animated ones */
enum { PAINT_RGB = N_COLORS, PAINT_ASTRAL, PAINT_PRISMATIC, PAINT_VOID };
RGB paint_rgb(int paint);
const char *paint_name(int paint);

enum { SLOT_TURRET = 0, SLOT_HULL, SLOT_TRACK, SLOT_BARREL, SLOT_GLOW, SLOT_AMBIENT, SLOT_ACCENT, N_SLOTS };

typedef struct {
    char id[12];
    char name[16];
    char lore[64];
    uint8_t turret, hull, track, barrel, glow, accent;
    uint64_t ambient;     /* AMB_* bitmask */
    bool hover;
    bool used;
} CustomTank;

typedef struct {
    int difficulty;       /* DIFF_* */
    double anim_speed;
    bool effects;
    double trail_life;
    double particle_density;
    int aim_guide;
    bool random_maps;
    bool sound;
    double wind;
    int home_mode;        /* index into HOME_ORDER */
} Settings;

typedef struct {
    int16_t tank;         /* runtime design index */
    uint8_t tank_color;   /* paint index */
    int16_t ammo;
    uint8_t shot_color;
} Loadout;

typedef struct {
    bool best;            /* "best" | "first" */
    uint8_t n_idx, n_custom, hp_idx, hp_custom;
    int16_t map;          /* -1 random */
    bool wind, special, ammo_fx, pick_maps, blessing;
} Rules;

/* story whispers + tutorial ticks */
enum { STORY_FIRST_BATTLE = 0, STORY_MASTER, STORY_ALIEN, STORY_MOTHERSHIP, STORY_HOARDERS,
       STORY_TUT1, STORY_TUT2, STORY_TUT3, STORY_TUT4, STORY_TUT5, STORY_TUT6, N_STORY };

typedef struct {
    uint32_t completed;                 /* tournament levels cleared (bit n-1) */
    uint8_t granted[(N_UIDS + 7) / 8];
    uint64_t mission_completed, mission_paid;
    uint16_t tickets_won;
    int8_t ticket_active;               /* -1 none */
    bool martyr_paid, martyr_won, martyr_seen, martyr_open;
    uint8_t martyr_phase;
    uint16_t story;
    uint8_t loaded_version;             /* format the save was read from (0: new save); 1 = migrated from DS 1.5.0 */
    double playtime;
    int32_t coins, credits;
    Loadout loadouts[2];
    Settings settings;
    bool has_rules[3];
    Rules rules[3];                     /* single, two, lan */
    CustomTank custom[CUSTOM_TANK_MAX];
    char warning[48];
    /* derived */
    uint8_t earned[(N_UIDS + 7) / 8];
} SaveData;

void settings_default(Settings *s);
void rules_default(Rules *r);
int  rules_n(const Rules *r);
int  rules_hp(const Rules *r);
int  rules_target(const Rules *r);
void rules_label(const Rules *r, char *out, int n);
double rules_wind_value(const Rules *r, const Settings *s);

void save_new(SaveData *s);
void save_refresh(SaveData *s);         /* recompute derived sets after any change */

/* custom tank runtime designs */
const TankDesign *design_ptr(int idx);
void custom_register(int slot, const CustomTank *ct);   /* slot: 0..CUSTOM_TANK_MAX+REMOTE-1 */
void custom_register_all(const SaveData *s);
int  custom_count(const SaveData *s);

bool save_is_unlocked(const SaveData *s, int uid);
bool save_grant(SaveData *s, int uid);
int  save_grant_all(SaveData *s);
bool save_level_unlocked(const SaveData *s, int n);
bool save_mission_unlocked(const SaveData *s, int n);
bool save_complete_level(SaveData *s, int n);      /* returns first clear */
bool save_complete_mission(SaveData *s, int n);
bool save_tank_unlocked(const SaveData *s, int design);
bool save_ammo_unlocked(const SaveData *s, int ammo);
bool save_map_unlocked(const SaveData *s, int map);
bool save_home_unlocked(const SaveData *s, int home);
bool save_master_unlocked(const SaveData *s);
bool save_paint_ok(const SaveData *s, int paint, bool hull);
bool save_missions_open(const SaveData *s);
bool save_tickets_open(const SaveData *s);
bool save_tickets_sold_out(const SaveData *s);
bool save_martyr_available(const SaveData *s);
bool save_workshop_open(const SaveData *s);
bool save_blessing_unlocked(const SaveData *s);
bool save_hear(SaveData *s, int story);            /* true exactly once */
int  save_progress_tier(const SaveData *s);
void save_earn(SaveData *s, int coins, int credits);
const char *save_shortfall(const SaveData *s, const ShopItem *it, char *buf, int n);
const char *save_buy(SaveData *s, const ShopItem *it, char *buf, int n);
int  save_buy_ticket(SaveData *s, const ShopItem *it, PyRandom *rng);   /* returns ticket index or -1 */
bool save_buy_martyr(SaveData *s, const ShopItem *it);
Loadout *save_loadout(SaveData *s, int i);          /* sanitised */
void save_sanitize(SaveData *s);
int  save_home_modes(const SaveData *s, int *out);  /* unlocked home screens in order */
void save_apply_settings_guard(SaveData *s);        /* locked content never leaks in through settings */

/* workshop */
typedef struct { uint8_t have[N_SLOTS][64]; int count[N_SLOTS]; } WorkshopParts;  /* indices into the component tables */
void workshop_components(const SaveData *s, WorkshopParts *wp);
const char *save_create_custom(SaveData *s, const CustomTank *draft, PyRandom *rng, char *buf, int n);
void save_delete_custom(SaveData *s, int slot);
void content_counts(const SaveData *s, int counts[6][2]);

/* cheats: returns success; msg receives the message */
bool redeem_cheat(SaveData *s, const char *code, char *msg, int n, int *activate_theme);

/* serialisation (versioned, little-endian, checksummed) */
int  save_serialize(const SaveData *s, uint8_t *buf, int cap);
bool save_deserialize(SaveData *s, const uint8_t *buf, int len);

#endif
