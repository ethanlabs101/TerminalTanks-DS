#include <nds.h>
#include <dswifi9.h>
#include <stdio.h>
#include <string.h>
#include "netplay.h"
#include "app.h"
#include "scenes.h"
#include "platform.h"

/* ------------------------------------------------------------------ encoding helpers */
void wr_u8(Wr *w, uint8_t v) { if (w->n < w->cap) w->p[w->n++] = v; }
void wr_u16(Wr *w, uint16_t v) { wr_u8(w, (uint8_t)v); wr_u8(w, (uint8_t)(v >> 8)); }
void wr_u32(Wr *w, uint32_t v) { wr_u16(w, (uint16_t)v); wr_u16(w, (uint16_t)(v >> 16)); }
void wr_f64(Wr *w, double v) { uint64_t u; memcpy(&u, &v, 8); wr_u32(w, (uint32_t)u); wr_u32(w, (uint32_t)(u >> 32)); }
void wr_bytes(Wr *w, const void *b, int n) { for (int i = 0; i < n; i++) wr_u8(w, ((const uint8_t *)b)[i]); }
void wr_str(Wr *w, const char *s) { int n = (int)strlen(s); if (n > 255) n = 255; wr_u8(w, (uint8_t)n); wr_bytes(w, s, n); }
uint8_t rd_u8(Rd *r) { if (r->i >= r->n) { r->bad = true; return 0; } return r->p[r->i++]; }
uint16_t rd_u16(Rd *r) { uint16_t a = rd_u8(r); return (uint16_t)(a | (rd_u8(r) << 8)); }
uint32_t rd_u32(Rd *r) { uint32_t a = rd_u16(r); return a | ((uint32_t)rd_u16(r) << 16); }
double rd_f64(Rd *r) { uint64_t lo = rd_u32(r), hi = rd_u32(r); uint64_t u = lo | (hi << 32); double v; memcpy(&v, &u, 8); return v; }
void rd_bytes(Rd *r, void *b, int n) { for (int i = 0; i < n; i++) ((uint8_t *)b)[i] = rd_u8(r); }
void rd_str(Rd *r, char *s, int cap) {
    int n = rd_u8(r);
    for (int i = 0; i < n; i++) {
        char c = (char)rd_u8(r);
        if (i < cap - 1) s[i] = c;
    }
    s[n < cap - 1 ? n : cap - 1] = 0;
}

/* ------------------------------------------------------------------ transport
 * A reliable ordered byte stream over dswifi's CMD (host -> guest) / REPLY (guest -> host) frames: every packet
 * carries the oldest unacknowledged bytes and an ack of what was received (go-back-N). See docs/MULTIPLAYER.md.
 *
 * Staying connected (protocol 2):
 * - the link is pumped once per frame by the app itself (net_frame), whatever screen is up, so pausing or a menu
 *   overlay never starves it;
 * - nothing is ever dropped on the floor: when the inbox is full the stream simply stops being acknowledged, and the
 *   other side resends later (aim echoes for the same tank are merged, so they can't fill it);
 * - silence is not a disconnect: after 3 s a banner says the game is waiting, and the stream carries on where it was
 *   when packets flow again;
 * - once a match has started, a lost association (out of range, a wireless reset) is repaired automatically: the
 *   guest scans for the same host and re-joins, the host takes it back, and both resume the stream from the last
 *   acknowledged byte, so no message is lost or doubled. A session id in every packet keeps other consoles out.
 *   Only after 90 s of trouble (or a QUIT) does the match end. */
#define PKT_SIZE 100
#define PKT_HDR 16
#define PKT_DATA (PKT_SIZE - PKT_HDR)
#define OUT_CAP 16384
#define AIM_BACKLOG 2048                 /* aim echoes are skipped while this much is waiting to be acknowledged */
#define RXQ 32
#define INBOX 64
#define VBL_S 60                         /* VBlanks per second */
#define T_WAIT (3 * VBL_S)               /* silence before the "waiting" banner */
#define T_FORCE (20 * VBL_S)             /* silence (either way) before the guest re-joins on its own */
#define T_KICK (25 * VBL_S)              /* silence before the host drops the guest's old association */
#define T_GIVEUP (90 * VBL_S)            /* trouble before the match is called off */
#define T_LOBBY (6 * VBL_S)              /* silence that ends a lobby link (no match yet) */

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t proto, n;
    uint32_t off, ack;
    uint16_t sid;
    uint16_t quiet;                      /* frames since the sender last heard us (so a one-way link is noticed) */
    uint8_t data[PKT_DATA];
} Pkt;
#define MAGIC 0x5454

enum { LINK_OK = 0, LINK_WAIT, LINK_REJOIN };
enum { RJ_MODE = 0, RJ_SCAN, RJ_CONNECT };

static bool g_init, g_init_ok;
static int g_state;
static bool g_host;
static char g_err[48];
static int g_channel = 6;
static uint8_t g_out[OUT_CAP];
static uint32_t g_base, g_wr;            /* stream offsets: acked, written */
static uint32_t g_recv;                  /* bytes received in order */
static uint8_t g_asm[3 + NET_MAX_MSG];   /* message being assembled */
static int g_asm_n;
static NetMsg g_inbox[INBOX];
static int g_ninbox;
static volatile uint8_t g_rxq[RXQ][PKT_SIZE];
static volatile uint8_t g_rxaid[RXQ];
static volatile int g_rx_w, g_rx_r;
static bool g_had_client, g_bound, g_match;
static uint16_t g_sid;                   /* session id (host picks it, the guest adopts it) */
static uint32_t g_heard, g_last_tx = 0xFFFFFFFF, g_trouble, g_peer_quiet, g_grace;
static int g_link, g_rj, g_kick_aid;
static uint32_t g_rj_t;
static uint8_t g_host_bssid[6];
static Wifi_AccessPoint g_aps[16];
static int g_naps;

static uint32_t vbl(void) { return plat_lines() / 263; }

static void rx_push(int aid, int base, int len) {
    int nxt = (g_rx_w + 1) % RXQ;
    if (nxt == g_rx_r || len < PKT_HDR) return;   /* full: the packet is resent anyway */
    uint8_t tmp[PKT_SIZE];
    memset(tmp, 0, sizeof tmp);
    Wifi_RxRawReadPacket((u32)base, (u32)(len < PKT_SIZE ? len : PKT_SIZE), tmp);
    memcpy((void *)g_rxq[g_rx_w], tmp, PKT_SIZE);
    g_rxaid[g_rx_w] = (uint8_t)aid;
    g_rx_w = nxt;
}
static void from_client(Wifi_MPPacketType type, int aid, int base, int len) { (void)type; rx_push(aid, base, len); }
static void from_host(Wifi_MPPacketType type, int base, int len) { (void)type; rx_push(0, base, len); }

bool net_available(void) {
    if (!g_init) {
        g_init = true;
        g_init_ok = Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY);
        if (!g_init_ok) snprintf(g_err, sizeof g_err, "WIRELESS COULD NOT START");
    }
    return g_init_ok;
}
int net_state(void) { return g_state; }
bool net_is_host(void) { return g_host; }
const char *net_error(void) { return g_err; }

static void reset_stream(void) {
    g_base = g_wr = g_recv = 0;
    g_asm_n = 0;
    g_ninbox = 0;
    g_rx_r = g_rx_w = 0;
    g_had_client = g_bound = g_match = false;
    g_sid = 0;
    g_link = LINK_OK;
    g_heard = vbl();
    g_grace = g_heard;
    g_peer_quiet = 0;
    g_kick_aid = 0;
}

static void wait_ready(void) {
    for (int i = 0; i < 180 && !Wifi_LibraryModeReady(); i++) swiWaitForVBlank();
}

bool net_host_start(const char *name) {
    if (!net_available()) return false;
    reset_stream();
    g_host = true;
    g_sid = (uint16_t)(plat_entropy() | 1);
    if (Wifi_MultiplayerHostMode(1, PKT_SIZE, PKT_SIZE) != 0) { snprintf(g_err, sizeof g_err, "HOST MODE FAILED"); return false; }
    Wifi_MultiplayerFromClientSetPacketHandler(from_client);
    wait_ready();
    Wifi_SetChannel(g_channel);
    Wifi_MultiplayerAllowNewClients(true);
    uint16_t nm[DSWIFI_BEACON_NAME_SIZE / 2];
    memset(nm, 0, sizeof nm);
    int l = 0;
    for (; name[l] && l < DSWIFI_BEACON_NAME_SIZE / 2; l++) nm[l] = (uint8_t)name[l];
    Wifi_MultiplayerHostName(nm, (u8)l);
    Wifi_BeaconStart("TerminalTanks", NET_GAME_ID);
    g_state = NS_HOSTING;
    return true;
}
int net_host_channel(void) { return g_channel; }
void net_host_set_channel(int ch) {
    g_channel = ch;
    if (g_state == NS_HOSTING) Wifi_SetChannel(ch);
}

bool net_scan_start(void) {
    if (!net_available()) return false;
    reset_stream();
    g_host = false;
    if (Wifi_MultiplayerClientMode(PKT_SIZE) != 0) { snprintf(g_err, sizeof g_err, "CLIENT MODE FAILED"); return false; }
    Wifi_MultiplayerFromHostSetPacketHandler(from_host);
    wait_ready();
    Wifi_ScanMode();
    g_state = NS_SCANNING;
    return true;
}

int net_scan_list(NetGame *out, int max) {
    if (g_state != NS_SCANNING) return 0;
    int n = Wifi_GetNumAP(), k = 0;
    g_naps = 0;
    for (int i = 0; i < n && k < max && g_naps < 16; i++) {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != 0) continue;
        if (!(ap.flags & WFLAG_APDATA_NINTENDO_TAG) || ap.nintendo.game_id != NET_GAME_ID) continue;
        g_aps[g_naps] = ap;
        NetGame *g = &out[k++];
        int l = 0;
        for (; l < ap.nintendo.name_len && l < 10 && l < (int)sizeof g->name - 1; l++) {
            uint16_t c = ap.nintendo.name[l];
            g->name[l] = c < 128 && c >= 32 ? (char)c : '?';
        }
        g->name[l] = 0;
        g->channel = ap.channel;
        g->players = ap.nintendo.players_current;
        g->max = ap.nintendo.players_max;
        g->rssi = ap.rssi;
        g->open = ap.nintendo.allows_connections;
        g->idx = g_naps++;
    }
    return k;
}

bool net_join(int idx) {
    if (g_state != NS_SCANNING || idx < 0 || idx >= g_naps) return false;
    reset_stream();
    memcpy(g_host_bssid, g_aps[idx].bssid, 6);
    Wifi_ConnectOpenAP(&g_aps[idx]);
    g_state = NS_CONNECTING;
    return true;
}

void net_close(void) {
    if (g_init_ok && g_state != NS_OFF) Wifi_IdleMode();
    g_state = NS_OFF;
    reset_stream();
}

void net_leave(void) {
    if (g_state == NS_CONNECTED && g_link == LINK_OK) {
        net_send(M_QUIT, NULL, 0);
        for (int i = 0; i < 20 && g_base < g_wr; i++) {   /* give the quit message a moment to get through */
            swiWaitForVBlank();
            net_pump();
        }
    }
    net_close();
}

bool net_dead(void) { return g_state == NS_DEAD; }
void net_set_match(bool on) { g_match = on; }

bool net_send(int type, const void *payload, int len) {
    if (g_state != NS_CONNECTED && g_state != NS_HOSTING && g_state != NS_CONNECTING) return false;
    uint32_t backlog = g_wr - g_base;
    if (type == M_AIM && (backlog > AIM_BACKLOG || g_link != LINK_OK)) return false;   /* a newer aim will follow */
    if (len > NET_MAX_MSG || (int)backlog + len + 3 > OUT_CAP) {
        plat_log("NET outbox full: message %d dropped", type);
        return false;
    }
    uint8_t hdr[3] = {(uint8_t)type, (uint8_t)len, (uint8_t)(len >> 8)};
    for (int i = 0; i < 3; i++) g_out[(g_wr++) % OUT_CAP] = hdr[i];
    for (int i = 0; i < len; i++) g_out[(g_wr++) % OUT_CAP] = ((const uint8_t *)payload)[i];
    return true;
}

/* true if the byte was taken; false: the inbox is full, so this byte (which would complete a message) waits */
static bool deliver_byte(uint8_t b) {
    if (g_asm_n >= 3) {
        int len = g_asm[1] | (g_asm[2] << 8);
        if (g_asm_n + 1 == 3 + len && g_ninbox >= INBOX) return false;
    } else if (g_asm_n == 2) {
        int len = g_asm[1] | (b << 8);
        if (len == 0 && g_ninbox >= INBOX) return false;
    }
    if (g_asm_n < (int)sizeof g_asm) g_asm[g_asm_n++] = b;
    if (g_asm_n < 3) return true;
    int len = g_asm[1] | (g_asm[2] << 8);
    if (len > NET_MAX_MSG) { g_asm_n = 0; return true; }
    if (g_asm_n < 3 + len) return true;
    int type = g_asm[0];
    NetMsg *m = NULL;
    if (type == M_AIM && len >= 1)            /* merge with a waiting aim of the same tank (nothing of it after) */
        for (int i = g_ninbox - 1; i >= 0; i--) {
            NetMsg *o = &g_inbox[i];
            if ((o->type == M_AIM || o->type == M_FIRE || o->type == M_SKIP) && o->len >= 1 && o->data[0] == g_asm[3]) {
                if (o->type == M_AIM) m = o;
                break;
            }
        }
    if (!m) m = &g_inbox[g_ninbox++];
    m->type = (uint8_t)type;
    m->len = (uint16_t)len;
    memcpy(m->data, g_asm + 3, (size_t)len);
    g_asm_n = 0;
    return true;
}

static void handle_pkt(const Pkt *p, int aid) {
    if (p->magic != MAGIC || p->proto != NET_PROTO) return;
    if (g_host) {
        /* in the lobby the guest has not learnt the session id yet (it reads it from our first packet) */
        if (p->sid != g_sid && !(p->sid == 0 && !g_match)) {
            if (g_match && aid > 0) g_kick_aid = aid;     /* not our guest: another console, or an old session */
            return;
        }
        g_bound = true;
    } else {
        if (g_sid == 0 && !g_recv && !g_base) g_sid = p->sid;    /* the first packet of this game */
        if (p->sid != g_sid) return;
    }
    g_heard = vbl();
    g_peer_quiet = p->quiet;
    if (p->ack > g_base && p->ack <= g_wr) g_base = p->ack;
    if (p->n && p->n <= PKT_DATA && p->off <= g_recv && p->off + p->n > g_recv) {
        uint32_t o = g_recv;
        for (; o < p->off + p->n; o++)
            if (!deliver_byte(p->data[o - p->off])) break;
        g_recv = o;
    }
}

/* -- keeping a started match alive */
static void rejoin_scan(void) {             /* start over as a fresh guest would (what works in the lobby) */
    Wifi_DisconnectAP();
    Wifi_MultiplayerClientMode(PKT_SIZE);
    Wifi_MultiplayerFromHostSetPacketHandler(from_host);
    g_rj = RJ_MODE;
    g_rj_t = vbl();
}

static void start_rejoin(const char *why) {
    if (g_link == LINK_REJOIN) return;
    plat_log("NET link lost (%s): reconnecting t=%lu", why, (unsigned long)vbl());
    if (g_link == LINK_OK) g_trouble = vbl();
    g_link = LINK_REJOIN;
    g_peer_quiet = 0;
    if (g_host) {
        u16 mask = Wifi_MultiplayerGetClientMask();   /* a guest that went quiet: let it associate afresh */
        for (int aid = 1; aid < 16; aid++) if (mask & (1u << aid)) Wifi_MultiplayerKickClientByAID(aid);
        Wifi_MultiplayerAllowNewClients(true);
    } else {
        rejoin_scan();
    }
}

static void rejoin_step(void) {
    uint32_t now = vbl();
    if (g_host) {                                  /* the beacon is still up: wait for our guest to come back */
        if (Wifi_MultiplayerGetNumClients() > 0 && vbl() - g_heard < T_WAIT) g_link = LINK_WAIT;   /* our guest is talking again */
        return;
    }
    if (g_rj == RJ_MODE) {
        if (Wifi_LibraryModeReady() || now - g_rj_t > 3 * VBL_S) {
            Wifi_ScanMode();
            g_rj = RJ_SCAN;
            g_rj_t = now;
        }
        return;
    }
    if (g_rj == RJ_SCAN) {
        int n = Wifi_GetNumAP();
        if (now - g_rj_t > 10 * VBL_S) { rejoin_scan(); return; }   /* nothing found for a while: start the scan over */
        for (int i = 0; i < n; i++) {
            Wifi_AccessPoint ap;
            if (Wifi_GetAPData(i, &ap) != 0 || memcmp(ap.bssid, g_host_bssid, 6)) continue;
            if (!(ap.flags & WFLAG_APDATA_NINTENDO_TAG) || !ap.nintendo.allows_connections) continue;
            plat_log("NET host found again: joining");
            Wifi_ConnectOpenAP(&ap);
            g_rj = RJ_CONNECT;
            g_rj_t = now;
            return;
        }
    } else {
        int st = Wifi_AssocStatus();
        if (st == ASSOCSTATUS_ASSOCIATED) {
            g_link = LINK_WAIT;
            g_heard = now;                         /* count silence afresh from the new association */
            g_grace = now + T_FORCE;               /* the host's "quiet" figure is stale until it hears us again */
            plat_log("NET re-joined the host");
            return;
        }
        if (st == ASSOCSTATUS_CANNOTCONNECT || now - g_rj_t > 8 * VBL_S) rejoin_scan();
    }
}

static bool associated(void) {
    return g_host ? Wifi_MultiplayerGetNumClients() > 0 : Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED;
}

static void link_check(void) {
    uint32_t now = vbl(), quiet = now - g_heard;
    if (!g_match) {                                /* lobby: no match to save, keep the old simple rule */
        if ((g_host && g_had_client && Wifi_MultiplayerGetNumClients() == 0) || quiet > T_LOBBY) g_state = NS_DEAD;
        return;
    }
    if (g_link == LINK_REJOIN) rejoin_step();
    else if (!associated()) start_rejoin(g_host ? "guest gone" : "host gone");
    else if (!g_host && g_peer_quiet > T_FORCE && (int32_t)(now - g_grace) > 0) start_rejoin("the host cannot hear us");
    else if (quiet > T_WAIT) {
        if (g_link == LINK_OK) { g_link = LINK_WAIT; g_trouble = now; plat_log("NET waiting: no packets for %lu frames", (unsigned long)quiet); }
        if (!g_host && quiet > T_FORCE) start_rejoin("silent too long");
        if (g_host && quiet > T_KICK) start_rejoin("guest silent too long");
    } else if (g_link != LINK_OK) {
        plat_log("NET link back after %lu frames", (unsigned long)(now - g_trouble));
        g_link = LINK_OK;
        if (g_host) Wifi_MultiplayerAllowNewClients(false);
    }
    if (g_link != LINK_OK && now - g_trouble > T_GIVEUP) {
        snprintf(g_err, sizeof g_err, "CONNECTION LOST");
        g_state = NS_DEAD;
    }
}

void net_pump(void) {
    if (g_state == NS_OFF || g_state == NS_SCANNING || g_state == NS_DEAD) return;
    if (g_state == NS_CONNECTING) {
        int st = Wifi_AssocStatus();
        if (st == ASSOCSTATUS_ASSOCIATED) { g_state = NS_CONNECTED; g_heard = vbl(); }
        else if (st == ASSOCSTATUS_CANNOTCONNECT) { g_state = NS_DEAD; snprintf(g_err, sizeof g_err, "COULD NOT CONNECT"); return; }
        else return;
    }
    if (g_state == NS_HOSTING) {
        if (Wifi_MultiplayerGetNumClients() > 0) {
            g_state = NS_CONNECTED;
            g_had_client = true;
            g_heard = vbl();
            Wifi_MultiplayerAllowNewClients(false);
        } else {
            return;
        }
    }
    while (g_rx_r != g_rx_w) {
        Pkt p;
        memcpy(&p, (const void *)g_rxq[g_rx_r], sizeof p);
        int aid = g_rxaid[g_rx_r];
        g_rx_r = (g_rx_r + 1) % RXQ;
        handle_pkt(&p, aid);
    }
    if (g_kick_aid) {
        Wifi_MultiplayerKickClientByAID(g_kick_aid);
        g_kick_aid = 0;
    }
    link_check();
    if (g_state == NS_DEAD) return;
    uint32_t now = vbl();
    if (now == g_last_tx) return;                  /* one packet per frame, however often this is called */
    g_last_tx = now;
    Pkt p;
    memset(&p, 0, sizeof p);
    p.magic = MAGIC;
    p.proto = NET_PROTO;
    p.sid = g_sid;
    p.off = g_base;
    p.ack = g_recv;
    p.quiet = (uint16_t)((now - g_heard) > 65535 ? 65535 : (now - g_heard));
    uint32_t avail = g_wr - g_base;
    p.n = (uint8_t)(avail > PKT_DATA ? PKT_DATA : avail);
    for (int i = 0; i < p.n; i++) p.data[i] = g_out[(g_base + (uint32_t)i) % OUT_CAP];
    if (g_host) Wifi_MultiplayerHostCmdTxFrame(&p, sizeof p);
    else Wifi_MultiplayerClientReplyTxFrame(&p, sizeof p);
}

void net_frame(void) {
    if (g_state != NS_OFF) net_pump();
}

const char *net_trouble_text(void) {
    static char buf[64];
    if (g_state != NS_CONNECTED || !g_match || g_link == LINK_OK) return NULL;
    int left = (int)((T_GIVEUP - (int)(vbl() - g_trouble)) / VBL_S);
    if (left < 0) left = 0;
    if (g_link == LINK_WAIT) snprintf(buf, sizeof buf, "WAITING FOR THE OTHER DS...  %d", left);
    else snprintf(buf, sizeof buf, "LINK LOST - RECONNECTING...  %d", left);
    return buf;
}

NetMsg *net_peek(uint32_t mask) {
    for (int i = 0; i < g_ninbox; i++) if (mask & NB(g_inbox[i].type)) return &g_inbox[i];
    return NULL;
}
void net_drop(NetMsg *m) {
    int i = (int)(m - g_inbox);
    if (i < 0 || i >= g_ninbox) return;
    memmove(&g_inbox[i], &g_inbox[i + 1], sizeof(NetMsg) * (size_t)(g_ninbox - i - 1));
    g_ninbox--;
}
bool net_take(int type, NetMsg *out) {
    NetMsg *m = net_peek(NB(type));
    if (!m) return false;
    if (out) *out = *m;
    net_drop(m);
    return true;
}
int net_inbox_count(void) { return g_ninbox; }

/* ------------------------------------------------------------------ battle glue */
static void battle_send(void *ctx, int type, int tank, double angle, double power) {
    (void)ctx;
    uint8_t buf[24];
    Wr w = {buf, 0, sizeof buf};
    wr_u8(&w, (uint8_t)tank);
    if (type == 's') { net_send(M_SKIP, buf, w.n); return; }
    if (type == 'f') plat_log("NET send fire tank %d a %.2f p %.2f", tank, angle, power);
    wr_f64(&w, angle);
    wr_f64(&w, power);
    net_send(type == 'f' ? M_FIRE : M_AIM, buf, w.n);
}

void net_attach_battle(Battle *b) {
    if (!b) return;
    b->send = battle_send;
    b->send_ctx = NULL;
}

bool net_alive_or_leave(void) {
    net_pump();
    if (net_take(M_QUIT, NULL)) {
        net_close();
        app_goto(scene_notice("OPPONENT LEFT", "The other player left the match."));
        return false;
    }
    if (net_dead()) {
        net_close();
        app_goto(scene_notice("CONNECTION LOST", "The other player disconnected."));
        return false;
    }
    return true;
}

bool net_battle_tick(Battle *b, float dt) {
    (void)dt;
    if (!net_alive_or_leave()) return false;
    Tank *cur = battle_current(b);
    if (b->phase == PH_AIMING && battle_control(b, cur) == CTL_REMOTE) {
        for (;;) {
            NetMsg *m = net_peek(NB(M_AIM) | NB(M_FIRE) | NB(M_SKIP));
            if (!m) break;
            Rd r = {m->data, m->len, 0, false};
            int tank = rd_u8(&r);
            if (tank != cur->index) {
                if (m->type == M_AIM) { net_drop(m); continue; }   /* a stale aim from an earlier turn */
                break;
            }
            int type = m->type;
            double a = 0, p = 0;
            if (type != M_SKIP) { a = rd_f64(&r); p = rd_f64(&r); }
            net_drop(m);
            if (type == M_SKIP) { battle_remote_skip(b, tank); break; }
            if (type == M_FIRE) { plat_log("NET recv fire tank %d a %.2f p %.2f", tank, a, p); battle_remote_fire(b, tank, a, p); break; }
            battle_remote_aim(b, tank, a, p);
        }
    }
    return true;
}

bool net_take_ready(void) { return net_take(M_READY, NULL); }
void net_send_ready(void) { net_send(M_READY, NULL, 0); }
