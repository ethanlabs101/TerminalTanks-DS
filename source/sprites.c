/* Story sprites: the Arcane Time Machine and the Martyr archmage (terminaltanks.py l.12477-12597). */
#include <math.h>
#include <string.h>
#include "sprites.h"

#define TAU_F 6.2831853f

void draw_artifact(Surf *cv, float cx, float cy, float s, float t) {
    int H = (int)(14 * s), Wd = (int)(8 * s);
    if (H < 8) H = 8;
    if (Wd < 5) Wd = 5;
    for (int k = 0; k < H; k++) {
        float f = fabsf(H / 2.0f - k - 0.5f) / (H / 2.0f);
        int hw = (int)(1 + f * Wd / 2);
        int y = (int)(cy - H / 2.0f + k);
        for (int dx = -hw; dx <= hw; dx++) {
            bool edge = (dx < 0 ? -dx : dx) >= hw || k == 0 || k == H - 1;
            RGB col = edge ? (RGB){255, 226, 150} : cmix((RGB){60, 40, 110}, (RGB){150, 110, 230}, 0.5f + 0.5f * fsin(t * 2 + k * 0.4f));
            cv_blend(cv, (int)cx + dx, y, col, edge ? 0.95f : 0.55f);
        }
    }
    int top = (int)(cy - H / 2.0f);
    for (int k = H / 2 - 2; k < H / 2 + 3; k++) cv_plot(cv, (int)cx, top + k, rgb15i(255, 236, 180));
    for (int k = 0; k < (int)(H * 0.22f); k++)
        for (int dx = -(k / 2); dx <= k / 2; dx++) cv_blend(cv, (int)cx + dx, top + 1 + (H / 2 - 1 - k), (RGB){255, 210, 120}, 0.6f);
    const float rings[2][3] = {{Wd * 1.4f, Wd * 0.5f, 0.8f}, {Wd * 1.1f, Wd * 0.42f, -1.2f}};
    for (int r = 0; r < 2; r++) {
        float rx = rings[r][0], ry = rings[r][1], sp = rings[r][2];
        for (int i = 0; i < 70; i++) {
            float ang = i / 70.0f * TAU_F + t * sp;
            cv_blendf(cv, cx + fcos(ang) * rx, cy + fsin(ang) * ry * (sp < 0 ? 1.4f : 1.0f) + fsin(ang * 3 + t) * 0.5f,
                      i % 2 ? (RGB){255, 226, 150} : (RGB){170, 120, 255}, 0.55f + 0.35f * fsin(ang * 5 + t * 3));
        }
    }
}

static char g_mage[32][24];
static bool g_mage_ok;
static void mput(int x, int y, char c) { if (x >= 0 && x < 24 && y >= 0 && y < 32) g_mage[y][x] = c; }
static void mage_build(void) {
    memset(g_mage, '.', sizeof g_mage);
    for (int y = 13; y < 32; y++) {
        int w = 4 + (int)((y - 13) * 0.33);
        for (int x = 12 - w; x <= 12 + w; x++) {
            bool edge = x == 12 - w || x == 12 + w;
            int ax = x - 12 < 0 ? 12 - x : x - 12;
            mput(x, y, edge ? 'G' : ((x - 12) % 5 == 0 ? 'L' : (ax < w - 1 ? 'M' : 'D')));
        }
    }
    for (int x = 5; x < 20; x++) { mput(x, 13, 'H'); mput(x, 14, 'D'); }
    for (int x = 8; x < 17; x++) mput(x, 19, 'G');
    for (int k = 0; k < 5; k++) { mput(15 + k, 16 - k / 2, 'M'); mput(15 + k, 17 - k / 2, 'D'); }
    mput(20, 15, 'g'); mput(19, 15, 'g');
    for (int y = 4; y < 32; y++) mput(20, y, (y - 4) % 7 == 0 ? 'g' : 'S');
    for (int y = 1; y < 14; y++) {
        int w = 1 + (int)((y - 1) * 0.5);
        for (int x = 12 - w; x <= 12 + w; x++) mput(x, y, (x == 12 - w || x == 12 + w) ? 'H' : 'D');
    }
    for (int y = 7; y < 13; y++) {
        int w = y < 11 ? 2 : 1;
        for (int x = 12 - w; x <= 12 + w; x++) mput(x, y, 'F');
    }
    mput(11, 9, 'E'); mput(13, 9, 'E');
    for (int y = 11; y < 22; y++) {
        int w = 3 - (y - 11) / 3;
        if (w < 0) w = 0;
        for (int x = 12 - w; x <= 12 + w; x++) mput(x, y, 'B');
    }
    static const int G[6][2] = {{8, 24}, {15, 26}, {10, 28}, {13, 22}, {7, 20}, {16, 30}};
    for (int i = 0; i < 6; i++) mput(G[i][0], G[i][1], 'g');
    g_mage_ok = true;
}

static RGB mage_pal(char ch) {
    switch (ch) {
    case 'D': return (RGB){30, 14, 66};
    case 'M': return (RGB){66, 34, 130};
    case 'L': return (RGB){112, 70, 204};
    case 'H': return (RGB){50, 24, 100};
    case 'G': return (RGB){232, 180, 70};
    case 'g': return (RGB){255, 240, 170};
    case 'F': return (RGB){6, 2, 16};
    case 'E': return (RGB){255, 250, 214};
    case 'B': return (RGB){226, 226, 246};
    default: return (RGB){140, 96, 52};
    }
}

void draw_mage(Surf *cv, float cx, float by, float s, float t, float alpha, bool aura) {
    if (!g_mage_ok) mage_build();
    float bob = fsin(t * 1.5f) * 0.8f * s;
    int sz = (int)ceilf(s);
    if (sz < 1) sz = 1;
    for (int row = 0; row < 32; row++)
        for (int col = 0; col < 24; col++) {
            char ch = g_mage[row][col];
            if (ch == '.') continue;
            RGB c = mage_pal(ch);
            if (ch == 'M' || ch == 'L') c = cmix(c, (RGB){150, 100, 240}, 0.25f + 0.25f * fsin(t * 2 + col * 0.6f + row * 0.35f));
            else if (ch == 'E') c = cmix(c, (RGB){255, 255, 255}, 0.5f + 0.5f * fsin(t * 6));
            else if (ch == 'B') c = cmix(c, (RGB){190, 190, 230}, 0.3f + 0.3f * fsin(t * 1.3f + row));
            float sway = row > 18 ? fsin(t * 2 + row * 0.25f) * 0.5f * (row / 32.0f) * s : 0.0f;
            float px = cx + (col - 12) * s + sway, py = by + bob + (31 - row) * s;
            for (int ox = 0; ox < sz; ox++)
                for (int oy = 0; oy < sz; oy++) cv_blend(cv, (int)px + ox, (int)py + oy, c, alpha);
        }
    float ox_ = cx + 8 * s, oy_ = by + bob + 30 * s;
    RGB orb = ELEMENTS[(int)(t * 0.8f) % 6].color;
    float r = fmaxf(2.0f, 2.6f * s);
    for (int dx = -(int)r - 1; dx < (int)r + 2; dx++)
        for (int dy = -(int)r - 1; dy < (int)r + 2; dy++) {
            float d = fhypot((float)dx, (float)dy);
            if (d <= r) cv_blend(cv, (int)ox_ + dx, (int)oy_ + dy, cmix(orb, (RGB){255, 255, 255}, 0.6f * (1 - d / r)), alpha * 0.95f);
            else if (d <= r + 1.5f) cv_blend(cv, (int)ox_ + dx, (int)oy_ + dy, orb, alpha * 0.3f);
        }
    if (aura) {
        static const int D[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int i = 0; i < 6; i++) {
            float ang = t * 1.1f + i * TAU_F / 6;
            float ex = cx + fcos(ang) * 14 * s, ey = by + 16 * s + fsin(ang) * 7 * s;
            for (int k = 0; k < 5; k++) cv_blendf(cv, ex + D[k][0], ey + D[k][1], ELEMENTS[i].color, alpha * (k == 0 ? 0.95f : 0.5f));
        }
    }
}

#include <stdlib.h>
void canvas_alloc(Canvas *c, int w, int h) {
    c->buf = (Px *)calloc((size_t)(w * h), sizeof(Px));
    c->s = (Surf){c->buf, w, h, w};
}
void canvas_free(Canvas *c) {
    free(c->buf);
    c->buf = NULL;
    c->s.px = NULL;
}
