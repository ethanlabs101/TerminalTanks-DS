#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "save.h"
#include "save_legacy.h"

/* ------------------------------------------------------------------ uid caches (built lazily) */
static int16_t g_design_uid[N_DESIGNS], g_ammo_uid[N_AMMOS], g_map_uid[N_MAPS_ALL], g_home_uid[N_HOME];
static int16_t g_level_uid[N_LEVELS + 1], g_mission_uid[N_MISSIONS + 1];
static int16_t g_uid_blessing = -1;
static bool g_ready;

static int lookup2(const char *prefix, const char *key) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s%s", prefix, key);
    return uid_index(buf);
}

static void ensure_tables(void) {
    if (g_ready) return;
    char buf[48];
    for (int i = 0; i < N_DESIGNS; i++) g_design_uid[i] = (int16_t)lookup2("tank:", DESIGNS[i].key);
    for (int i = 0; i < N_AMMOS; i++) g_ammo_uid[i] = (int16_t)lookup2("ammo:", AMMOS[i].key);
    for (int i = 0; i < N_MAPS_ALL; i++) g_map_uid[i] = (int16_t)lookup2("map:", MAPDEFS[i].key);
    for (int i = 0; i < N_HOME; i++) {
        char low[32];
        int k = 0;
        for (const char *p = HOME_ORDER[i]; *p && k < 31; p++) low[k++] = (char)tolower((unsigned char)*p);
        low[k] = 0;
        g_home_uid[i] = (int16_t)lookup2("theme:", low);
    }
    for (int n = 1; n <= N_LEVELS; n++) { snprintf(buf, sizeof buf, "level:%d", n); g_level_uid[n] = (int16_t)uid_index(buf); }
    for (int n = 1; n <= N_MISSIONS; n++) { snprintf(buf, sizeof buf, "mission:%d", n); g_mission_uid[n] = (int16_t)uid_index(buf); }
    g_uid_blessing = (int16_t)uid_index("ammo:blessing");
    g_ready = true;
}

static bool bit(const uint8_t *s, int i) { return i >= 0 && (s[i >> 3] >> (i & 7)) & 1; }
static void setbit(uint8_t *s, int i) { if (i >= 0) s[i >> 3] |= (uint8_t)(1u << (i & 7)); }

/* ------------------------------------------------------------------ paints */
RGB paint_rgb(int p) {
    if (p >= 0 && p < N_COLORS) return COLOR_CHOICES[p].c;
    if (p == PAINT_RGB) return (RGB){250, 3, 251};
    if (p == PAINT_ASTRAL) return (RGB){251, 4, 250};
    if (p == PAINT_PRISMATIC) return (RGB){252, 6, 247};
    if (p == PAINT_VOID) return (RGB){247, 2, 252};
    return COLOR_CHOICES[0].c;
}
const char *paint_name(int p) {
    static const char *const extra[4] = {"RGB", "ASTRAL", "PRISMATIC", "VOID"};
    if (p >= 0 && p < N_COLORS) return COLOR_CHOICES[p].name;
    if (p >= N_COLORS && p < N_PAINTS) return extra[p - N_COLORS];
    return "?";
}

/* ------------------------------------------------------------------ settings & rules */
void settings_default(Settings *s) {
    s->difficulty = DIFF_NORMAL;
    s->anim_speed = 1.0;
    s->effects = true;
    s->trail_life = 0.55;
    s->particle_density = 1.0;
    s->aim_guide = 1;
    s->random_maps = false;
    s->sound = false;
    s->wind = 6.0;
    s->home_mode = HOME_CLASSIC;
}

static const int ROUND_PRESETS[5] = {3, 5, 7, 10, 0};
static const int HP_PRESETS[6] = {1, 3, 5, 7, 10, 0};

void rules_default(Rules *r) {
    memset(r, 0, sizeof(*r));
    r->best = true;
    r->n_idx = 0; r->n_custom = 4; r->hp_idx = 1; r->hp_custom = 4;
    r->map = -1;
    r->wind = true; r->special = false; r->ammo_fx = true; r->pick_maps = true; r->blessing = false;
}
int rules_n(const Rules *r) { return ROUND_PRESETS[r->n_idx % 5] ? ROUND_PRESETS[r->n_idx % 5] : r->n_custom; }
int rules_hp(const Rules *r) { return HP_PRESETS[r->hp_idx % 6] ? HP_PRESETS[r->hp_idx % 6] : r->hp_custom; }
int rules_target(const Rules *r) { return r->best ? rules_n(r) / 2 + 1 : rules_n(r); }
void rules_label(const Rules *r, char *out, int n) { snprintf(out, (size_t)n, "%s %d", r->best ? "BEST OF" : "FIRST TO", rules_n(r)); }
double rules_wind_value(const Rules *r, const Settings *s) { return !r->wind ? 0.0 : (s->wind > 0 ? s->wind : 5.0); }

/* ------------------------------------------------------------------ custom designs */
static TankDesign g_custom[CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS];
static char g_custom_mask[CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS][67];
static char g_custom_name[CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS][16];
static char g_custom_lore[CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS][64];
static char g_custom_key[CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS][20];

const TankDesign *design_ptr(int idx) {
    if (idx >= 0 && idx < N_DESIGNS) return &DESIGNS[idx];
    if (idx >= N_DESIGNS && idx < N_DESIGNS_RT && g_custom[idx - N_DESIGNS].key) return &g_custom[idx - N_DESIGNS];
    return &DESIGNS[0];
}

void custom_register(int slot, const CustomTank *ct) {
    if (slot < 0 || slot >= CUSTOM_TANK_MAX + CUSTOM_REMOTE_SLOTS) return;
    TankDesign *d = &g_custom[slot];
    int t = ct->turret < TUR_COUNT ? ct->turret : 0, h = ct->hull < HUL_COUNT ? ct->hull : 0, k = ct->track < TRK_COUNT ? ct->track : 0;
    snprintf(g_custom_mask[slot], 67, "%s%s%s", TURRET_ROWS[t], HULL_ROWS[h], TRACK_ROWS[k]);
    /* name: upper-case letters, digits, space and '-', max 14 (design_from_custom l.4834) */
    int n = 0;
    for (const char *p = ct->name; *p && n < 14; p++) {
        char c = (char)toupper((unsigned char)*p);
        if (isalnum((unsigned char)c) || c == ' ' || c == '-') g_custom_name[slot][n++] = c;
    }
    g_custom_name[slot][n] = 0;
    while (n > 0 && g_custom_name[slot][n - 1] == ' ') g_custom_name[slot][--n] = 0;
    char *nm = g_custom_name[slot];
    while (*nm == ' ') nm++;
    if (!*nm) strcpy(g_custom_name[slot], "CUSTOM"), nm = g_custom_name[slot];
    snprintf(g_custom_lore[slot], 64, "%s", ct->lore[0] ? ct->lore : "A tank of your own design.");
    snprintf(g_custom_key[slot], 20, "custom:%s", ct->id);
    d->key = g_custom_key[slot];
    d->name = nm;
    d->blurb = g_custom_lore[slot];
    d->mask = g_custom_mask[slot];
    d->barrel = ct->barrel < BARREL_COUNT ? ct->barrel : 0;
    d->glow = ct->glow < GLOW_COUNT ? ct->glow : 0;
    d->ambient = ct->ambient ? ct->ambient : 1ull;
    d->accent = ct->accent < N_COLORS ? COLOR_CHOICES[ct->accent].c : (RGB){236, 242, 250};
    d->has_paint = 0;
    d->turret = (int8_t)t; d->hull = (int8_t)h; d->track = (int8_t)k;
    d->hover = ct->hover ? 3.0 : 0.0;
}

void custom_register_all(const SaveData *s) {
    for (int i = 0; i < CUSTOM_TANK_MAX; i++) {
        if (s->custom[i].used) custom_register(i, &s->custom[i]);
        else g_custom[i].key = NULL;
    }
}

int custom_count(const SaveData *s) {
    int n = 0;
    for (int i = 0; i < CUSTOM_TANK_MAX; i++) n += s->custom[i].used;
    return n;
}

/* ------------------------------------------------------------------ save basics */
void save_new(SaveData *s) {
    ensure_tables();
    memset(s, 0, sizeof(*s));
    s->ticket_active = -1;
    s->loadouts[0] = (Loadout){0, 0, AMMO_STANDARD, 6};                          /* ranger CYAN standard YELLOW */
    s->loadouts[1] = (Loadout){0, (uint8_t)color_index_by_name("ORANGE"), AMMO_STANDARD, (uint8_t)color_index_by_name("MAGENTA")};
    settings_default(&s->settings);
    for (int i = 0; i < 3; i++) rules_default(&s->rules[i]);
    save_refresh(s);
}

void save_refresh(SaveData *s) {
    ensure_tables();
    memset(s->earned, 0, sizeof s->earned);
    for (int n = 1; n <= N_LEVELS; n++)
        if (s->completed & (1u << (n - 1)))
            for (int k = 0; k < 6; k++) setbit(s->earned, LEVEL_REWARDS[n - 1][k]);
    for (int n = 1; n <= N_MISSIONS; n++)
        if (s->mission_completed & (1ull << (n - 1)))
            for (int k = 0; k < MISSIONS[n - 1].nrewards; k++) setbit(s->earned, MISSIONS[n - 1].rewards[k]);
    custom_register_all(s);
}

bool save_level_unlocked(const SaveData *s, int n) {
    ensure_tables();
    return n == 1 || (n >= 2 && (s->completed & (1u << (n - 2)))) || bit(s->granted, g_level_uid[n]);
}
bool save_mission_unlocked(const SaveData *s, int n) {
    ensure_tables();
    return n == 1 || (n >= 2 && (s->mission_completed & (1ull << (n - 2)))) || bit(s->granted, g_mission_uid[n]);
}
bool save_tickets_sold_out(const SaveData *s) { return (s->tickets_won & ((1u << N_TICKETS) - 1)) == ((1u << N_TICKETS) - 1); }
bool save_blessing_unlocked(const SaveData *s) { return s->martyr_won; }

bool save_is_unlocked(const SaveData *s, int uid) {
    ensure_tables();
    if (uid < 0) return false;
    if (uid == UID_TICKET) return save_tickets_sold_out(s);
    if (uid == UID_TIMEMACHINE) return s->martyr_won;
    if (uid == g_uid_blessing) return save_blessing_unlocked(s);
    for (int n = 2; n <= N_LEVELS; n++) if (g_level_uid[n] == uid) return save_level_unlocked(s, n);
    for (int n = 2; n <= N_MISSIONS; n++) if (g_mission_uid[n] == uid) return save_mission_unlocked(s, n);
    return bit(s->granted, uid) || bit(s->earned, uid);
}

bool save_grant(SaveData *s, int uid) {
    if (uid < 0 || uid == g_uid_blessing || uid >= N_UNLOCKABLES || save_is_unlocked(s, uid)) return false;
    setbit(s->granted, uid);
    save_sanitize(s);
    return true;
}

int save_grant_all(SaveData *s) {
    int n = 0;
    for (int u = 0; u < N_UNLOCKABLES; u++) n += save_grant(s, u);
    return n;
}

bool save_complete_level(SaveData *s, int n) {
    bool first = !(s->completed & (1u << (n - 1)));
    s->completed |= 1u << (n - 1);
    save_refresh(s);
    save_sanitize(s);
    return first;
}

bool save_complete_mission(SaveData *s, int n) {
    bool first = !(s->mission_completed & (1ull << (n - 1)));
    s->mission_completed |= 1ull << (n - 1);
    s->mission_paid |= 1ull << (n - 1);
    save_refresh(s);
    save_sanitize(s);
    return first;
}

static bool uid_has(const SaveData *s, int uid) { return bit(s->granted, uid) || bit(s->earned, uid); }

bool save_tank_unlocked(const SaveData *s, int d) {
    ensure_tables();
    if (d >= 0 && d < 5) return true;
    if (d >= DESIGN_CUSTOM_FIRST && d < DESIGN_CUSTOM_FIRST + CUSTOM_TANK_MAX) return s->custom[d - DESIGN_CUSTOM_FIRST].used;
    if (d >= DESIGN_REMOTE_FIRST && d < N_DESIGNS_RT) return false;
    return d >= 0 && d < N_DESIGNS && uid_has(s, g_design_uid[d]);
}

bool save_ammo_unlocked(const SaveData *s, int a) {
    ensure_tables();
    if (a == AMMO_BLESSING) return save_blessing_unlocked(s);
    if (a >= 0 && a < 5) return true;
    return a >= 0 && a < N_AMMOS_PUBLIC && uid_has(s, g_ammo_uid[a]);
}

bool save_map_unlocked(const SaveData *s, int m) {
    ensure_tables();
    if (m >= 0 && m < N_MAPS_LIB) return true;
    return m >= 0 && m < N_MAPS_ALL && save_is_unlocked(s, g_map_uid[m]);
}

bool save_home_unlocked(const SaveData *s, int h) {
    ensure_tables();
    return h == HOME_CLASSIC || (h > 0 && h < N_HOME && save_is_unlocked(s, g_home_uid[h]));
}

int save_home_modes(const SaveData *s, int *out) {
    int n = 0;
    for (int h = 0; h < N_HOME; h++) if (save_home_unlocked(s, h)) out[n++] = h;
    return n;
}

bool save_master_unlocked(const SaveData *s) { return save_is_unlocked(s, UID_MASTER_DIFFICULTY); }

bool save_paint_ok(const SaveData *s, int p, bool hull) {
    if (p < N_COLORS) return true;
    if (p == PAINT_RGB) return save_is_unlocked(s, hull ? UID_RGB_HULL : UID_RGB_SHELL);
    if (p == PAINT_ASTRAL) return save_is_unlocked(s, UID_PAINT_ASTRAL);
    if (p == PAINT_PRISMATIC) return save_is_unlocked(s, UID_PAINT_PRISMATIC);
    if (p == PAINT_VOID) return save_is_unlocked(s, UID_PAINT_VOID);
    return false;
}

bool save_missions_open(const SaveData *s) {
    ensure_tables();
    if ((s->completed & (1u << (N_LEVELS - 1))) || s->mission_completed || s->mission_paid) return true;
    for (int n = 2; n <= N_MISSIONS; n++) if (bit(s->granted, g_mission_uid[n])) return true;
    return false;
}

bool save_tickets_open(const SaveData *s) {
    return ((s->completed & (1u << (N_LEVELS - 1))) && (s->mission_completed & (1ull << (N_MISSIONS - 1)))) || s->tickets_won ||
           s->ticket_active >= 0 || s->martyr_open || s->martyr_paid || s->martyr_won;
}

bool save_martyr_available(const SaveData *s) { return save_tickets_sold_out(s) || s->martyr_open; }

bool save_workshop_open(const SaveData *s) {
    return (s->completed & (1u << 4)) || custom_count(s) > 0 || (s->completed & ~0x1Fu);
}

bool save_hear(SaveData *s, int story) {
    if (s->story & (1u << story)) return false;
    s->story |= (uint16_t)(1u << story);
    return true;
}

int save_progress_tier(const SaveData *s) {
    if (s->martyr_won) return 4;
    if (save_tickets_sold_out(s)) return 3;
    if (s->mission_completed & (1ull << (N_MISSIONS - 1))) return 2;
    return (s->completed & (1u << (N_LEVELS - 1))) ? 1 : 0;
}

void save_earn(SaveData *s, int coins, int credits) {
    s->coins += coins > 0 ? coins : 0;
    s->credits += credits > 0 ? credits : 0;
}

const char *save_shortfall(const SaveData *s, const ShopItem *it, char *buf, int n) {
    if (s->coins < it->coins) { snprintf(buf, (size_t)n, "NEED %d MORE COINS", (int)(it->coins - s->coins)); return buf; }
    if (s->credits < it->credits) { snprintf(buf, (size_t)n, "NEED %d MORE CREDITS", (int)(it->credits - s->credits)); return buf; }
    return NULL;
}

const char *save_buy(SaveData *s, const ShopItem *it, char *buf, int n) {
    if (save_is_unlocked(s, it->uid)) { snprintf(buf, (size_t)n, "ALREADY OWNED"); return buf; }
    const char *why = save_shortfall(s, it, buf, n);
    if (why) return why;
    s->coins -= it->coins;
    s->credits -= it->credits;
    setbit(s->granted, it->uid);
    save_sanitize(s);
    return NULL;
}

int save_buy_ticket(SaveData *s, const ShopItem *it, PyRandom *rng) {
    char buf[40];
    if (!save_tickets_open(s) || s->ticket_active >= 0 || save_tickets_sold_out(s) || save_shortfall(s, it, buf, sizeof buf)) return -1;
    /* pool = sorted(keys - won): sort by key string, like the original */
    int pool[N_TICKETS], n = 0;
    for (int i = 0; i < N_TICKETS; i++) if (!(s->tickets_won & (1u << i))) pool[n++] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(TICKETS[pool[j - 1]].key, TICKETS[pool[j]].key) > 0; j--) { int t = pool[j]; pool[j] = pool[j - 1]; pool[j - 1] = t; }
    s->coins -= it->coins;
    s->credits -= it->credits;
    s->ticket_active = (int8_t)pool[rng_below(rng, (uint32_t)n)];
    return s->ticket_active;
}

bool save_buy_martyr(SaveData *s, const ShopItem *it) {
    char buf[40];
    if (!save_martyr_available(s) || s->martyr_paid || save_shortfall(s, it, buf, sizeof buf)) return false;
    s->coins -= it->coins;
    s->credits -= it->credits;
    s->martyr_paid = true;
    return true;
}

Loadout *save_loadout(SaveData *s, int i) {
    Loadout *lo = &s->loadouts[i];
    if (!save_tank_unlocked(s, lo->tank)) lo->tank = 0;
    if (!save_ammo_unlocked(s, lo->ammo)) lo->ammo = AMMO_STANDARD;
    if (lo->tank_color >= N_PAINTS || !save_paint_ok(s, lo->tank_color, true)) lo->tank_color = 0;
    if (lo->shot_color >= N_PAINTS || !save_paint_ok(s, lo->shot_color, false)) lo->shot_color = 6;
    return lo;
}

void save_sanitize(SaveData *s) {
    save_loadout(s, 0);
    save_loadout(s, 1);
}

void save_apply_settings_guard(SaveData *s) {
    if (!save_master_unlocked(s) && s->settings.difficulty == DIFF_MASTER) s->settings.difficulty = DIFF_NORMAL;
    if (!save_home_unlocked(s, s->settings.home_mode)) s->settings.home_mode = HOME_CLASSIC;
}

/* ------------------------------------------------------------------ workshop (l.4816-4887) */
static const char *const FREE_TURRET[] = {"dome", "box", "twin", "wide", "pillbox"};
static const char *const FREE_HULL[] = {"plain", "plate", "ramp", "wide", "hex"};
static const char *const FREE_TRACK[] = {"std", "full", "round", "tread"};
static const char *const FREE_BARREL[] = {"single", "double", "heavy"};
static const char *const FREE_GLOW[] = {"none", "pulse", "blink"};
static const char *const FREE_AMB[] = {"none", "sparks", "mist"};

static int find_name(const char *const *tab, int n, const char *k) {
    for (int i = 0; i < n; i++) if (!strcmp(tab[i], k)) return i;
    return -1;
}

void workshop_components(const SaveData *s, WorkshopParts *wp) {
    bool have[N_SLOTS][64];
    memset(have, 0, sizeof have);
    for (unsigned i = 0; i < 5; i++) have[SLOT_TURRET][find_name(TURRET_NAMES, TUR_COUNT, FREE_TURRET[i])] = true;
    for (unsigned i = 0; i < 5; i++) have[SLOT_HULL][find_name(HULL_NAMES, HUL_COUNT, FREE_HULL[i])] = true;
    for (unsigned i = 0; i < 4; i++) have[SLOT_TRACK][find_name(TRACK_NAMES, TRK_COUNT, FREE_TRACK[i])] = true;
    for (unsigned i = 0; i < 3; i++) have[SLOT_BARREL][find_name(BARREL_NAMES, BARREL_COUNT, FREE_BARREL[i])] = true;
    for (unsigned i = 0; i < 3; i++) have[SLOT_GLOW][find_name(GLOW_NAMES, GLOW_COUNT, FREE_GLOW[i])] = true;
    for (unsigned i = 0; i < 3; i++) have[SLOT_AMBIENT][find_name(AMBIENT_NAMES, AMB_COUNT, FREE_AMB[i])] = true;
    for (int d = 0; d < N_DESIGNS; d++) {
        if (!(d < 5 || save_tank_unlocked(s, d))) continue;
        const TankDesign *td = &DESIGNS[d];
        if (td->turret >= 0) {
            have[SLOT_TURRET][td->turret] = true;
            have[SLOT_HULL][td->hull] = true;
            have[SLOT_TRACK][td->track] = true;
        }
        have[SLOT_BARREL][td->barrel] = true;
        have[SLOT_GLOW][td->glow] = true;
        for (int k = 0; k < AMB_COUNT; k++) if (td->ambient & (1u << k)) have[SLOT_AMBIENT][k] = true;
    }
    for (int c = 0; c < N_COLORS; c++) have[SLOT_ACCENT][c] = true;
    const int tot[N_SLOTS] = {TUR_COUNT, HUL_COUNT, TRK_COUNT, BARREL_COUNT, GLOW_COUNT, AMB_COUNT, N_COLORS};
    for (int sl = 0; sl < N_SLOTS; sl++) {
        wp->count[sl] = 0;
        for (int i = 0; i < tot[sl]; i++) if (have[sl][i]) wp->have[sl][wp->count[sl]++] = (uint8_t)i;
    }
}

static bool wp_has(const WorkshopParts *wp, int slot, int idx) {
    for (int i = 0; i < wp->count[slot]; i++) if (wp->have[slot][i] == idx) return true;
    return false;
}

const char *save_create_custom(SaveData *s, const CustomTank *draft, PyRandom *rng, char *buf, int n) {
    if (custom_count(s) >= CUSTOM_TANK_MAX) { snprintf(buf, (size_t)n, "WORKSHOP FULL - DELETE A TANK FIRST"); return buf; }
    int cost = CUSTOM_TANK_FEE + (draft->hover ? HOVER_COINS : 0), cred = draft->hover ? HOVER_CREDITS : 0;
    if ((int)s->coins < cost) { snprintf(buf, (size_t)n, "NEED %d MORE COINS", (int)(cost - s->coins)); return buf; }
    if ((int)s->credits < cred) { snprintf(buf, (size_t)n, "NEED %d MORE CREDITS FOR HOVER", (int)(cred - s->credits)); return buf; }
    WorkshopParts wp;
    workshop_components(s, &wp);
    bool ok = wp_has(&wp, SLOT_TURRET, draft->turret) && wp_has(&wp, SLOT_HULL, draft->hull) && wp_has(&wp, SLOT_TRACK, draft->track) &&
              wp_has(&wp, SLOT_BARREL, draft->barrel) && wp_has(&wp, SLOT_GLOW, draft->glow) && wp_has(&wp, SLOT_ACCENT, draft->accent);
    for (int k = 0; k < AMB_COUNT; k++) if ((draft->ambient & (1u << k)) && !wp_has(&wp, SLOT_AMBIENT, k)) ok = false;
    if (!ok) { snprintf(buf, (size_t)n, "A CHOSEN PART IS NOT UNLOCKED"); return buf; }
    int slot = -1;
    for (int i = 0; i < CUSTOM_TANK_MAX; i++) if (!s->custom[i].used) { slot = i; break; }
    CustomTank ct = *draft;
    static const char hex[] = "0123456789abcdef";
    ct.id[0] = 'c';
    for (int i = 1; i <= 10; i++) ct.id[i] = hex[rng_below(rng, 16)];
    ct.id[11] = 0;
    ct.used = true;
    s->custom[slot] = ct;
    custom_register(slot, &ct);
    s->coins -= cost;
    s->credits -= cred;
    return NULL;
}

void save_delete_custom(SaveData *s, int slot) {
    if (slot < 0 || slot >= CUSTOM_TANK_MAX) return;
    /* keep creation order: shift later tanks down (the original stores a list) */
    for (int i = slot; i < CUSTOM_TANK_MAX - 1; i++) s->custom[i] = s->custom[i + 1];
    memset(&s->custom[CUSTOM_TANK_MAX - 1], 0, sizeof(CustomTank));
    for (int l = 0; l < 2; l++) {
        int16_t *t = &s->loadouts[l].tank;
        if (*t == DESIGN_CUSTOM_FIRST + slot) *t = 0;
        else if (*t > DESIGN_CUSTOM_FIRST + slot && *t < DESIGN_CUSTOM_FIRST + CUSTOM_TANK_MAX) (*t)--;
    }
    custom_register_all(s);
    save_sanitize(s);
}

void content_counts(const SaveData *s, int c[6][2]) {
    int tanks = 0, tt = 0, ammo = 0, at = 0;
    for (int d = 0; d < N_DESIGNS; d++) {
        if (d >= DESIGNS_SECRET_FIRST && d < DESIGNS_SECRET_FIRST + DESIGNS_SECRET_COUNT) continue;
        tt++;
        tanks += save_tank_unlocked(s, d);
    }
    for (int a = 0; a < N_AMMOS_PUBLIC; a++) {
        if (a >= AMMOS_SECRET_FIRST && a < AMMOS_SECRET_FIRST + AMMOS_SECRET_COUNT) continue;
        at++;
        ammo += save_ammo_unlocked(s, a);
    }
    int maps = N_MAPS_LIB;
    for (int m = N_MAPS_LIB; m < N_MAPS_ALL; m++) maps += save_map_unlocked(s, m);
    int homes[N_HOME];
    WorkshopParts wp;
    workshop_components(s, &wp);
    int have = 0, total = TUR_COUNT + HUL_COUNT + TRK_COUNT + BARREL_COUNT + GLOW_COUNT + AMB_COUNT;
    for (int sl = 0; sl < SLOT_ACCENT; sl++) have += wp.count[sl];
    c[0][0] = tanks; c[0][1] = tt;
    c[1][0] = ammo; c[1][1] = at;
    c[2][0] = maps; c[2][1] = N_MAPS_ALL;
    c[3][0] = save_home_modes(s, homes); c[3][1] = N_HOME;
    c[4][0] = have; c[4][1] = total;
    c[5][0] = custom_count(s); c[5][1] = CUSTOM_TANK_MAX;
}

/* ------------------------------------------------------------------ cheats (l.5370) */
bool redeem_cheat(SaveData *s, const char *code, char *msg, int n, int *activate_theme) {
    char norm[64];
    int k = 0;
    for (const char *p = code; *p && k < 63; p++) if (!isspace((unsigned char)*p)) norm[k++] = (char)tolower((unsigned char)*p);
    norm[k] = 0;
    *activate_theme = -1;
    const Cheat *c = NULL;
    for (int i = 0; i < N_CHEATS; i++) if (!strcmp(CHEATS[i].code, norm)) c = &CHEATS[i];
    if (!c) { snprintf(msg, (size_t)n, "UNKNOWN CODE"); return false; }
    if (!strcmp(c->code, "unlocktimemachine")) {
        if (s->martyr_open && s->martyr_paid) { snprintf(msg, (size_t)n, "THE TIME MACHINE IS ALREADY UNLOCKED"); return true; }
        s->martyr_open = s->martyr_paid = true;
        snprintf(msg, (size_t)n, "TIME MACHINE UNLOCKED - FIND IT IN THE SHOP (EXTRAS) AND PRESS A");
        return true;
    }
    if (c->everything) {
        int got = save_grant_all(s);
        if (got) snprintf(msg, (size_t)n, "ALL UNLOCKABLES GRANTED - %d NEW", got);
        else snprintf(msg, (size_t)n, "EVERYTHING IS ALREADY UNLOCKED");
        return true;
    }
    if (c->level) {
        if (save_level_unlocked(s, c->level)) { snprintf(msg, (size_t)n, "LEVEL %d IS ALREADY OPEN", c->level); return true; }
        save_grant(s, c->grants[0]);
        snprintf(msg, (size_t)n, "LEVEL %d OPENED - CLEAR IT TO EARN ITS REWARDS", c->level);
        return true;
    }
    int got = 0;
    for (int i = 0; i < c->ngrants; i++) got += save_grant(s, c->grants[i]);
    if (!got) { snprintf(msg, (size_t)n, "ALREADY UNLOCKED"); return true; }
    if (c->activate_theme >= 0) *activate_theme = c->activate_theme;
    snprintf(msg, (size_t)n, "%s", c->text && c->text[0] ? c->text : "UNLOCKED");
    return true;
}

/* ------------------------------------------------------------------ serialisation */
#define SAVE_MAGIC 0x53445454u   /* "TTDS" */
#define SAVE_VERSION 2      /* 2: Terminal Tanks DS 2.0 (version 1 = DS 1.5.0, migrated on load) */

typedef struct { uint8_t *b; int n, cap; bool ok; } W;
static void wbytes(W *w, const void *p, int n) {
    if (w->n + n > w->cap) { w->ok = false; return; }
    memcpy(w->b + w->n, p, (size_t)n);
    w->n += n;
}
static void wu8(W *w, uint32_t v) { uint8_t x = (uint8_t)v; wbytes(w, &x, 1); }
static void wu16(W *w, uint32_t v) { wu8(w, v & 0xff); wu8(w, v >> 8); }
static void wu32(W *w, uint32_t v) { wu16(w, v & 0xffff); wu16(w, v >> 16); }
static void wu64(W *w, uint64_t v) { wu32(w, (uint32_t)v); wu32(w, (uint32_t)(v >> 32)); }
static void wdbl(W *w, double d) { uint64_t u; memcpy(&u, &d, 8); wu64(w, u); }
static void wstr(W *w, const char *s, int n) { char tmp[64] = {0}; strncpy(tmp, s, (size_t)n - 1); wbytes(w, tmp, n); }

typedef struct { const uint8_t *b; int n, pos; bool ok; } R;
static void rbytes(R *r, void *p, int n) {
    if (r->pos + n > r->n) { r->ok = false; memset(p, 0, (size_t)n); return; }
    memcpy(p, r->b + r->pos, (size_t)n);
    r->pos += n;
}
static uint32_t ru8(R *r) { uint8_t x; rbytes(r, &x, 1); return x; }
static uint32_t ru16(R *r) { uint32_t a = ru8(r); return a | (ru8(r) << 8); }
static uint32_t ru32(R *r) { uint32_t a = ru16(r); return a | (ru16(r) << 16); }
static uint64_t ru64(R *r) { uint64_t a = ru32(r); return a | ((uint64_t)ru32(r) << 32); }
static double rdbl(R *r) { uint64_t u = ru64(r); double d; memcpy(&d, &u, 8); return d; }
static void rstr(R *r, char *s, int n) { rbytes(r, s, n); s[n - 1] = 0; }

static uint32_t checksum(const uint8_t *b, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void w_rules(W *w, const Rules *r) {
    wu8(w, r->best); wu8(w, r->n_idx); wu8(w, r->n_custom); wu8(w, r->hp_idx); wu8(w, r->hp_custom);
    wu16(w, (uint16_t)r->map); wu8(w, r->wind); wu8(w, r->special); wu8(w, r->ammo_fx); wu8(w, r->pick_maps); wu8(w, r->blessing);
}
static void r_rules(R *r, Rules *x) {
    x->best = ru8(r); x->n_idx = (uint8_t)(ru8(r) % 5); x->n_custom = (uint8_t)ru8(r); x->hp_idx = (uint8_t)(ru8(r) % 6); x->hp_custom = (uint8_t)ru8(r);
    x->map = (int16_t)ru16(r); x->wind = ru8(r); x->special = ru8(r); x->ammo_fx = ru8(r); x->pick_maps = ru8(r); x->blessing = ru8(r);
    if (x->n_custom < 1) x->n_custom = 1;
    if (x->n_custom > 25) x->n_custom = 25;
    if (x->hp_custom < 1) x->hp_custom = 1;
    if (x->hp_custom > 50) x->hp_custom = 50;
    if (x->map < -1 || x->map >= N_MAPS_ALL) x->map = -1;
}

/* old position -> new position (out-of-range or vanished -> -1; callers sanitise) */
static int legacy_map(const short *tab, int n, int i) { return i >= 0 && i < n ? tab[i] : -1; }

int save_serialize(const SaveData *s, uint8_t *buf, int cap) {
    W w = {buf, 0, cap, true};
    wu32(&w, SAVE_MAGIC);
    wu16(&w, SAVE_VERSION);
    wu16(&w, N_UIDS);
    wu32(&w, s->completed);
    wbytes(&w, s->granted, sizeof s->granted);
    wu64(&w, s->mission_completed);
    wu64(&w, s->mission_paid);
    wu16(&w, s->tickets_won);
    wu8(&w, (uint8_t)s->ticket_active);
    wu8(&w, s->martyr_paid); wu8(&w, s->martyr_won); wu8(&w, s->martyr_seen); wu8(&w, s->martyr_open); wu8(&w, s->martyr_phase);
    wu16(&w, s->story);
    wdbl(&w, s->playtime);
    wu32(&w, (uint32_t)s->coins);
    wu32(&w, (uint32_t)s->credits);
    for (int i = 0; i < 2; i++) {
        wu16(&w, (uint16_t)s->loadouts[i].tank); wu8(&w, s->loadouts[i].tank_color);
        wu16(&w, (uint16_t)s->loadouts[i].ammo); wu8(&w, s->loadouts[i].shot_color);
    }
    const Settings *st = &s->settings;
    wu8(&w, (uint8_t)st->difficulty); wdbl(&w, st->anim_speed); wu8(&w, st->effects); wdbl(&w, st->trail_life);
    wdbl(&w, st->particle_density); wu8(&w, (uint8_t)st->aim_guide); wu8(&w, st->random_maps); wu8(&w, st->sound);
    wdbl(&w, st->wind); wu8(&w, (uint8_t)st->home_mode);
    for (int i = 0; i < 3; i++) { wu8(&w, s->has_rules[i]); w_rules(&w, &s->rules[i]); }
    for (int i = 0; i < CUSTOM_TANK_MAX; i++) {
        const CustomTank *c = &s->custom[i];
        wu8(&w, c->used);
        if (!c->used) continue;
        wstr(&w, c->id, 12); wstr(&w, c->name, 16); wstr(&w, c->lore, 64);
        wu8(&w, c->turret); wu8(&w, c->hull); wu8(&w, c->track); wu8(&w, c->barrel); wu8(&w, c->glow); wu8(&w, c->accent);
        wu64(&w, c->ambient); wu8(&w, c->hover);
    }
    if (!w.ok) return -1;
    uint32_t h = checksum(buf, w.n);
    wu32(&w, h);
    return w.ok ? w.n : -1;
}

bool save_deserialize(SaveData *s, const uint8_t *buf, int len) {
    save_new(s);
    if (len < 12) return false;
    uint32_t stored = (uint32_t)buf[len - 4] | ((uint32_t)buf[len - 3] << 8) | ((uint32_t)buf[len - 2] << 16) | ((uint32_t)buf[len - 1] << 24);
    if (stored != checksum(buf, len - 4)) { snprintf(s->warning, sizeof s->warning, "save file damaged - starting fresh"); return false; }
    R r = {buf, len - 4, 0, true};
    if (ru32(&r) != SAVE_MAGIC) { snprintf(s->warning, sizeof s->warning, "not a save file - starting fresh"); return false; }
    int ver = (int)ru16(&r);
    int nuids = (int)ru16(&r);
    bool v1 = ver == 1 && nuids == LEGACY_V1_N_UID;            /* DS 1.5.0: positions are remapped by name below */
    if (!v1 && (ver != SAVE_VERSION || nuids != N_UIDS)) { snprintf(s->warning, sizeof s->warning, "save from another version - fresh start"); return false; }
    SaveData t;
    save_new(&t);
    t.completed = ru32(&r) & ((1u << N_LEVELS) - 1);
    if (v1) {
        uint8_t old[(LEGACY_V1_N_UID + 7) / 8];
        rbytes(&r, old, sizeof old);
        for (int u = 0; u < LEGACY_V1_N_UID; u++)
            if ((old[u >> 3] >> (u & 7)) & 1) {
                int nu = LEGACY_V1_UID[u];
                if (nu >= 0 && nu < N_UIDS) setbit(t.granted, nu);
            }
    } else {
        rbytes(&r, t.granted, sizeof t.granted);
    }
    t.mission_completed = ru64(&r) & ((1ull << N_MISSIONS) - 1);
    t.mission_paid = ru64(&r) & ((1ull << N_MISSIONS) - 1);
    t.tickets_won = (uint16_t)(ru16(&r) & ((1u << N_TICKETS) - 1));
    t.ticket_active = (int8_t)ru8(&r);
    if (t.ticket_active >= N_TICKETS) t.ticket_active = -1;
    t.martyr_paid = ru8(&r); t.martyr_won = ru8(&r); t.martyr_seen = ru8(&r); t.martyr_open = ru8(&r);
    t.martyr_phase = (uint8_t)ru8(&r);
    if (t.martyr_phase > 3) t.martyr_phase = 0;
    t.story = (uint16_t)ru16(&r);
    t.playtime = rdbl(&r);
    if (!(t.playtime >= 0)) t.playtime = 0;
    int32_t coins = (int32_t)ru32(&r), credits = (int32_t)ru32(&r);
    t.coins = coins > 0 ? coins : 0;
    t.credits = credits > 0 ? credits : 0;
    for (int i = 0; i < 2; i++) {
        t.loadouts[i].tank = (int16_t)ru16(&r); t.loadouts[i].tank_color = (uint8_t)ru8(&r);
        t.loadouts[i].ammo = (int16_t)ru16(&r); t.loadouts[i].shot_color = (uint8_t)ru8(&r);
        if (v1) {
            t.loadouts[i].tank = legacy_map(LEGACY_V1_DESIGN, LEGACY_V1_N_DESIGN, t.loadouts[i].tank);
            t.loadouts[i].ammo = legacy_map(LEGACY_V1_AMMO, LEGACY_V1_N_AMMO, t.loadouts[i].ammo);
        }
    }
    Settings *st = &t.settings;
    st->difficulty = (int)(ru8(&r) % 4); st->anim_speed = rdbl(&r); st->effects = ru8(&r); st->trail_life = rdbl(&r);
    st->particle_density = rdbl(&r); st->aim_guide = (int)(ru8(&r) % 3); st->random_maps = ru8(&r); st->sound = ru8(&r);
    st->wind = rdbl(&r); st->home_mode = (int)ru8(&r);
    if (v1) st->home_mode = legacy_map(LEGACY_V1_HOME, LEGACY_V1_N_HOME, st->home_mode);
    if (st->home_mode < 0) st->home_mode = HOME_CLASSIC;
    if (st->home_mode >= N_HOME) st->home_mode = HOME_CLASSIC;
    if (!(st->anim_speed > 0.1 && st->anim_speed < 4)) st->anim_speed = 1.0;
    if (!(st->wind >= 0 && st->wind <= 10)) st->wind = 6.0;
    for (int i = 0; i < 3; i++) {
        t.has_rules[i] = ru8(&r);
        if (v1) {                       /* the stored map index is a 1.5.0 position */
            Rules *x = &t.rules[i];
            x->best = ru8(&r); x->n_idx = (uint8_t)(ru8(&r) % 5); x->n_custom = (uint8_t)ru8(&r); x->hp_idx = (uint8_t)(ru8(&r) % 6);
            x->hp_custom = (uint8_t)ru8(&r);
            int m = (int16_t)ru16(&r);
            x->map = (int16_t)(m < 0 ? -1 : legacy_map(LEGACY_V1_MAP, LEGACY_V1_N_MAP, m));
            x->wind = ru8(&r); x->special = ru8(&r); x->ammo_fx = ru8(&r); x->pick_maps = ru8(&r); x->blessing = ru8(&r);
            if (x->n_custom < 1) x->n_custom = 1;
            if (x->n_custom > 25) x->n_custom = 25;
            if (x->hp_custom < 1) x->hp_custom = 1;
            if (x->hp_custom > 50) x->hp_custom = 50;
            if (x->map < -1 || x->map >= N_MAPS_ALL) x->map = -1;
        } else {
            r_rules(&r, &t.rules[i]);
        }
    }
    for (int i = 0; i < CUSTOM_TANK_MAX && r.ok; i++) {
        CustomTank *c = &t.custom[i];
        c->used = ru8(&r);
        if (!c->used) continue;
        rstr(&r, c->id, 12); rstr(&r, c->name, 16); rstr(&r, c->lore, 64);
        c->turret = (uint8_t)ru8(&r); c->hull = (uint8_t)ru8(&r); c->track = (uint8_t)ru8(&r); c->barrel = (uint8_t)ru8(&r);
        c->glow = (uint8_t)ru8(&r); c->accent = (uint8_t)ru8(&r);
        c->ambient = v1 ? ru32(&r) : ru64(&r);
        c->hover = ru8(&r);
        if (v1) {
            c->turret = (uint8_t)legacy_map(LEGACY_V1_TURRET, LEGACY_V1_N_TURRET, c->turret);
            c->hull = (uint8_t)legacy_map(LEGACY_V1_HULL, LEGACY_V1_N_HULL, c->hull);
            c->track = (uint8_t)legacy_map(LEGACY_V1_TRACK, LEGACY_V1_N_TRACK, c->track);
            c->barrel = (uint8_t)legacy_map(LEGACY_V1_BARREL, LEGACY_V1_N_BARREL, c->barrel);
            c->glow = (uint8_t)legacy_map(LEGACY_V1_GLOW, LEGACY_V1_N_GLOW, c->glow);
            c->accent = (uint8_t)legacy_map(LEGACY_V1_COLOR, LEGACY_V1_N_COLOR, c->accent);
            uint64_t a = 0;
            for (int k = 0; k < LEGACY_V1_N_AMB; k++)
                if ((c->ambient >> k) & 1) {
                    int nk = LEGACY_V1_AMB[k];
                    if (nk >= 0) a |= 1ull << nk;
                }
            c->ambient = a;
        }
    }
    if (!r.ok) { snprintf(s->warning, sizeof s->warning, "save file truncated - starting fresh"); return false; }
    /* chain rule (l.4999): a level / mission only counts if it was reachable */
    uint32_t ok = 0;
    for (int n = 1; n <= N_LEVELS; n++) {
        bool reach = n == 1 || (ok & (1u << (n - 2))) || bit(t.granted, g_level_uid[n]);
        if ((t.completed & (1u << (n - 1))) && reach) ok |= 1u << (n - 1);
    }
    t.completed = ok;
    uint64_t okm = 0;
    for (int n = 1; n <= N_MISSIONS; n++) {
        bool reach = n == 1 || (okm & (1ull << (n - 2))) || bit(t.granted, g_mission_uid[n]);
        if ((t.mission_completed & (1ull << (n - 1))) && reach) okm |= 1ull << (n - 1);
    }
    t.mission_completed = okm;
    t.mission_paid |= okm;
    for (int u = N_UNLOCKABLES; u < N_UIDS; u++) t.granted[u >> 3] &= (uint8_t)~(1u << (u & 7));
    t.loaded_version = (uint8_t)ver;
    *s = t;
    save_refresh(s);
    if (s->martyr_won) for (int i = 0; i < N_MARTYR_REWARDS; i++) setbit(s->granted, MARTYR_REWARDS[i]);
    save_sanitize(s);
    save_apply_settings_guard(s);
    return true;
}
