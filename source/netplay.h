/* Local wireless multiplayer over dswifi (DS-to-DS, no router). Replaces the original's TCP NetLink / LanHost /
 * LanBrowser (terminaltanks.py l.10669-10946) with the same message semantics: a reliable, ordered stream of typed
 * messages ("hello", "welcome", "start", "aim", "fire", "skip", "ready", "map", "rps", "rpsfirst", "quit") on top of
 * the DS multiplayer CMD/REPLY frames. See docs/MULTIPLAYER.md. */
#ifndef TT_NETPLAY_H
#define TT_NETPLAY_H
#include <stdbool.h>
#include <stdint.h>
#include "../core/battle.h"

#define NET_PROTO 2                     /* 2: session id in every packet, reconnecting links */
#define NET_GAME_ID 0x0054544Bu         /* 'TTK' */
#define NET_MAX_MSG 400

enum { M_HELLO = 1, M_WELCOME, M_START, M_AIM, M_FIRE, M_SKIP, M_READY, M_MAP, M_RPS, M_RPSFIRST, M_QUIT, M_REFUSE };
enum { NS_OFF = 0, NS_HOSTING, NS_SCANNING, NS_CONNECTING, NS_CONNECTED, NS_DEAD };

typedef struct { uint8_t type; uint16_t len; uint8_t data[NET_MAX_MSG]; } NetMsg;

bool net_available(void);                  /* wifi initialised (false: show an error) */
int  net_state(void);
bool net_is_host(void);
const char *net_error(void);

bool net_host_start(const char *name);     /* become the access point, advertise the game */
int  net_host_channel(void);
void net_host_set_channel(int ch);
bool net_scan_start(void);
typedef struct { char name[24]; int channel, players, max, rssi; bool open; int idx; } NetGame;
int  net_scan_list(NetGame *out, int max);
bool net_join(int idx);
void net_close(void);                       /* back to idle (sends nothing) */
void net_leave(void);                       /* polite: "quit", flush briefly, close */

void net_pump(void);                        /* receive + send (at most one packet per frame); scenes may call it */
void net_frame(void);                       /* the app calls this every frame: the link never starves */
void net_set_match(bool on);                /* a match has started: from now on a lost link is re-joined */
const char *net_trouble_text(void);         /* NULL, or the banner to show while the link is being repaired */
bool net_dead(void);
bool net_send(int type, const void *payload, int len);
NetMsg *net_peek(uint32_t type_mask);       /* oldest message of any type in the mask (bit = 1 << type) */
void net_drop(NetMsg *m);
bool net_take(int type, NetMsg *out);       /* removes the oldest message of this type */
int  net_inbox_count(void);
#define NB(t) (1u << (t))

/* battle glue (BattleScene._net_tick) */
void net_attach_battle(Battle *b);
bool net_battle_tick(Battle *b, float dt);  /* false: the link is gone and the scene changed */
bool net_alive_or_leave(void);              /* pumps; on quit/dead shows the notice and returns false */
bool net_take_ready(void);
void net_send_ready(void);

/* little helpers for encoding */
typedef struct { uint8_t *p; int n, cap; } Wr;
typedef struct { const uint8_t *p; int n, i; bool bad; } Rd;
void wr_u8(Wr *w, uint8_t v);
void wr_u16(Wr *w, uint16_t v);
void wr_u32(Wr *w, uint32_t v);
void wr_f64(Wr *w, double v);
void wr_bytes(Wr *w, const void *b, int n);
void wr_str(Wr *w, const char *s);
uint8_t rd_u8(Rd *r);
uint16_t rd_u16(Rd *r);
uint32_t rd_u32(Rd *r);
double rd_f64(Rd *r);
void rd_bytes(Rd *r, void *b, int n);
void rd_str(Rd *r, char *s, int cap);

#endif
