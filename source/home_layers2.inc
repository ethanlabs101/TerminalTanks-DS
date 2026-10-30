/* The ten 2.0 home-screen layers (reference/tanks/art/home_layers.py _L_abyss .. _L_armada), drawn like the original on
 * a half-block pixel layer over the home screen: 2 pixels per text row, starting from the cells' current backgrounds
 * so everything alpha-blends, written back as "upper half block" cells. Included by backdrop.c (needs LCtx). */

#define HP_MAXW 64
#define HP_MAXH 64
typedef struct {
    LCtx *c;
    int W, H;
    RGB rows[HP_MAXH][HP_MAXW], base[HP_MAXH][HP_MAXW];
} HPx;
static HPx g_hp;

static float hp_mod(float a, float b) { float r = fmodf(a, b); return r < 0 ? r + b : r; }
static float hp_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static HPx *hp_begin(LCtx *c) {
    HPx *P = &g_hp;
    P->c = c;
    P->W = c->cols < HP_MAXW ? c->cols : HP_MAXW;
    P->H = c->rows * 2 < HP_MAXH ? c->rows * 2 : HP_MAXH;
    for (int r = 0; r < P->H / 2; r++)
        for (int x = 0; x < P->W; x++) {
            RGB b = L_bg(c, x, r);
            P->rows[2 * r][x] = P->rows[2 * r + 1][x] = P->base[2 * r][x] = P->base[2 * r + 1][x] = b;
        }
    return P;
}
/* integer core: pixel (xi, yi), alpha k in 0..256 (the original skips alphas <= 0.02) */
static inline void hp_blendi(HPx *P, int xi, int yi, RGB c, int k) {
#ifdef HP_COUNT
    g_blends++;
#endif
    if ((unsigned)xi >= (unsigned)P->W || (unsigned)yi >= (unsigned)P->H || k <= 5) return;
    if (k > 256) k = 256;
    RGB *d = &P->rows[yi][xi];
    d->r = (uint8_t)(d->r + (((c.r - d->r) * k) >> 8));
    d->g = (uint8_t)(d->g + (((c.g - d->g) * k) >> 8));
    d->b = (uint8_t)(d->b + (((c.b - d->b) * k) >> 8));
}
static inline int hp_k(float a) { return a >= 1 ? 256 : a <= 0 ? 0 : (int)(a * 256); }
static inline int hp_i(float v) { return v < 0 ? -1 : (int)v; }       /* int() for the canvas: negatives are off-canvas */
static inline void hp_blend(HPx *P, float x, float y, RGB c, float a) { hp_blendi(P, hp_i(x), hp_i(y), c, hp_k(a)); }
static inline void hp_put(HPx *P, float x, float y, RGB c) {
    int xi = hp_i(x), yi = hp_i(y);
    if ((unsigned)xi < (unsigned)P->W && (unsigned)yi < (unsigned)P->H) P->rows[yi][xi] = c;
}
/* lines in 16.16 fixed point */
static void hp_line(HPx *P, float x0, float y0, float x1, float y1, RGB c, float a) {
    int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1, k = hp_k(a);
    int32_t fx = (int32_t)(x0 * 65536), fy = (int32_t)(y0 * 65536);
    int32_t dx = (int32_t)((x1 - x0) * 65536) / n, dy = (int32_t)((y1 - y0) * 65536) / n;
    for (int j = 0; j <= n; j++, fx += dx, fy += dy) hp_blendi(P, fx < 0 ? -1 : fx >> 16, fy < 0 ? -1 : fy >> 16, c, k);
}
static inline uint32_t hp_isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}
/* discs in 1/16 px: distances squared, an integer square root only in the glow ring */
static void hp_disc(HPx *P, float cx, float cy, float r, RGB c, float a, float glow, RGB gc) {
    float R = r + glow;
    int32_t cx16 = (int32_t)(cx * 16), cy16 = (int32_t)(cy * 16), r16 = (int32_t)(r * 16), R16 = (int32_t)(R * 16), g16 = (int32_t)(glow * 16);
    int32_t r2 = r16 * r16, R2 = R16 * R16;
    int k = hp_k(a), kg = (k * 115) >> 8;                                  /* a * 0.45 */
    int x0 = (int)(cx - R) - 1, x1 = (int)(cx + R) + 2, y0 = (int)(cy - R) - 1, y1 = (int)(cy + R) + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > P->W) x1 = P->W;
    if (y1 > P->H) y1 = P->H;
    for (int y = y0; y < y1; y++) {
        int32_t dy = y * 16 + 8 - cy16;
        for (int x = x0; x < x1; x++) {
            int32_t dx = x * 16 + 8 - cx16, d2 = dx * dx + dy * dy;
            if (d2 <= r2) hp_blendi(P, x, y, c, k);
            else if (g16 > 0 && d2 <= R2) {
                int32_t q = 256 - (((int32_t)hp_isqrt((uint32_t)d2) - r16) << 8) / g16;   /* 1 - (d - r) / glow, Q8 */
                if (q > 0) hp_blendi(P, x, y, gc, (kg * q * q) >> 16);
            }
        }
    }
}
/* fn(u, v, ctx, &col, &alpha) -> false: skip; u, v in -1..1 across the ellipse */
typedef bool (*HpFn)(float u, float v, const void *ctx, RGB *col, float *a);
static void hp_ellipse(HPx *P, float cx, float cy, float rx, float ry, HpFn fn, const void *ctx) {
    float irx = 1.0f / fmaxf(0.5f, rx), iry = 1.0f / fmaxf(0.5f, ry);
    for (int y = (int)(cy - ry) - 1; y < (int)(cy + ry) + 2; y++)
        for (int x = (int)(cx - rx) - 1; x < (int)(cx + rx) + 2; x++) {
            float u = (x + 0.5f - cx) * irx, v = (y + 0.5f - cy) * iry;
            RGB col;
            float a;
            if (u * u + v * v <= 1.0f && fn(u, v, ctx, &col, &a)) hp_blend(P, x, y, col, a);
        }
}
/* filled polygon (even-odd scanline) */
static void hp_poly(HPx *P, const float (*pts)[2], int n, RGB c, float a) {
    if (n < 3) return;
    float mn = pts[0][1], mx = pts[0][1];
    for (int i = 1; i < n; i++) { if (pts[i][1] < mn) mn = pts[i][1]; if (pts[i][1] > mx) mx = pts[i][1]; }
    int y0 = (int)mn, y1 = (int)mx + 1;
    for (int y = y0 > 0 ? y0 : 0; y < (y1 + 1 < P->H ? y1 + 1 : P->H); y++) {
        float yc = y + 0.5f, xs[96];
        int nx = 0;
        for (int i = 0; i < n && nx < 96; i++) {
            float ax = pts[i][0], ay = pts[i][1], bx = pts[(i + 1) % n][0], by = pts[(i + 1) % n][1];
            if ((ay <= yc && yc < by) || (by <= yc && yc < ay)) xs[nx++] = ax + (yc - ay) * (bx - ax) / (by - ay);
        }
        for (int i = 1; i < nx; i++) { float v = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
        for (int k = 0; k + 1 < nx; k += 2)
            for (int x = (int)(xs[k] + 0.5f), xe = (int)(xs[k + 1] + 0.5f), kk = hp_k(a); x < xe; x++) hp_blendi(P, x, y, c, kk);
    }
}
static void hp_quad(HPx *P, float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, RGB c, float a) {
    const float q[4][2] = {{ax, ay}, {bx, by}, {cx, cy}, {dx, dy}};
    hp_poly(P, q, 4, c, a);
}
static void hp_tri(HPx *P, float ax, float ay, float bx, float by, float cx, float cy, RGB c, float a) {
    const float q[3][2] = {{ax, ay}, {bx, by}, {cx, cy}};
    hp_poly(P, q, 3, c, a);
}
static void hp_commit(HPx *P) {
    for (int r = 0; r < P->H / 2; r++)
        for (int x = 0; x < P->W; x++) {
            RGB t = P->rows[2 * r][x], b = P->rows[2 * r + 1][x];
            if (rgb_eq(t, P->base[2 * r][x]) && rgb_eq(b, P->base[2 * r + 1][x])) continue;
            L_put(P->c, x, r, 0x2580, t, &b);
        }
}
/* ---- affine stamps: a section of the picture that does not move (mountains, gates, skylines) is a per-pixel affine
 * map out = under * k + C (every primitive lerps toward a fixed colour, so k is the same for all channels). It is
 * measured once by drawing the section over black and over white, cached, and replayed each frame in integers. */
#define HP_STAMPS 12
typedef struct { const void *owner; int id, W, H; uint8_t k[HP_MAXH][HP_MAXW]; RGB c[HP_MAXH][HP_MAXW]; } HpStamp;
static HpStamp g_stamps[HP_STAMPS];
static int g_stamp_next;
typedef void (*HpSection)(HPx *P, const void *ctx);
static void hp_stamp(HPx *P, int id, HpSection fn, const void *ctx, float scale) {
    const void *owner = P->c->th;
    HpStamp *st = NULL;
    for (int i = 0; i < HP_STAMPS; i++)
        if (g_stamps[i].owner == owner && g_stamps[i].id == id && g_stamps[i].W == P->W && g_stamps[i].H == P->H) st = &g_stamps[i];
    if (!st) {
        static HPx tmp;
        st = &g_stamps[g_stamp_next];
        g_stamp_next = (g_stamp_next + 1) % HP_STAMPS;
        st->owner = owner; st->id = id; st->W = P->W; st->H = P->H;
        tmp.c = P->c; tmp.W = P->W; tmp.H = P->H;
        for (int y = 0; y < P->H; y++) for (int x = 0; x < P->W; x++) tmp.rows[y][x] = (RGB){0, 0, 0};
        fn(&tmp, ctx);
        for (int y = 0; y < P->H; y++) for (int x = 0; x < P->W; x++) st->c[y][x] = tmp.rows[y][x];
        for (int y = 0; y < P->H; y++) for (int x = 0; x < P->W; x++) tmp.rows[y][x] = (RGB){255, 255, 255};
        fn(&tmp, ctx);
        for (int y = 0; y < P->H; y++)
            for (int x = 0; x < P->W; x++) {
                RGB w = tmp.rows[y][x], bl = st->c[y][x];
                int k = ((w.r - bl.r) + (w.g - bl.g) + (w.b - bl.b)) / 3;
                st->k[y][x] = (uint8_t)(k < 0 ? 0 : k > 255 ? 255 : k);
            }
    }
    int sc = hp_k(scale);                                                 /* scale < 1 fades the whole section */
    int ks_tab_dummy = 0; (void)ks_tab_dummy;
    for (int y = 0; y < P->H; y++)
        for (int x = 0; x < P->W; x++) {
            int k = st->k[y][x];
            RGB cc = st->c[y][x];
            if (k == 255 && !cc.r && !cc.g && !cc.b) continue;            /* untouched */
            RGB *d = &P->rows[y][x];
            int kk = k, cr = cc.r, cg = cc.g, cb = cc.b;
            if (sc < 256) { kk = 255 - (((255 - k) * sc) >> 8); cr = (cr * sc) >> 8; cg = (cg * sc) >> 8; cb = (cb * sc) >> 8; }   /* k' = 1-(1-k)s, C' = Cs */
            int r = ((d->r * kk) >> 8) + cr, g = ((d->g * kk) >> 8) + cg, b = ((d->b * kk) >> 8) + cb;
            d->r = (uint8_t)(r > 255 ? 255 : r); d->g = (uint8_t)(g > 255 ? 255 : g); d->b = (uint8_t)(b > 255 ? 255 : b);
        }
}

/* a flat-coloured ellipse in integers */
static void hp_ellipse_flat(HPx *P, float cx, float cy, float rx, float ry, RGB col, int k) {
    int32_t irx = (int32_t)(65536 / fmaxf(0.5f, rx)), iry = (int32_t)(65536 / fmaxf(0.5f, ry)), cx8 = (int32_t)(cx * 256), cy8 = (int32_t)(cy * 256);
    for (int y = (int)(cy - ry) - 1; y < (int)(cy + ry) + 2; y++) {
        int32_t v = (((y * 256 + 128) - cy8) * iry) >> 16, v2 = (v * v) >> 8;
        if (v2 > 256) continue;
        for (int x = (int)(cx - rx) - 1; x < (int)(cx + rx) + 2; x++) {
            int32_t u = (((x * 256 + 128) - cx8) * irx) >> 16;
            if (((u * u) >> 8) + v2 <= 256) hp_blendi(P, x, y, col, k);
        }
    }
}
static inline RGB hp_mixk(RGB a, RGB b, int k) {
    RGB o = {(uint8_t)(a.r + (((b.r - a.r) * k) >> 8)), (uint8_t)(a.g + (((b.g - a.g) * k) >> 8)), (uint8_t)(a.b + (((b.b - a.b) * k) >> 8))};
    return o;
}
static inline RGB hp_mix(RGB a, RGB b, float k) { return hp_mixk(a, b, hp_k(k)); }
static const RGB HP_WHITE = {255, 255, 255};

/* ---- BIOLUME TRENCH ---------------------------------------------------------------------------------------- */
typedef struct { RGB hue; } HueCtx;
static bool abyss_bell(float u, float v, const void *x, RGB *col, float *a) {
    const HueCtx *h = x;
    if (v > 0.2f) return false;
    float d = u * u + v * v;
    float canal = fabsf(4 * u * v * (u * u - v * v)) < 0.18f * d * d ? 0.25f : 0.0f;   /* |sin(4 atan2(v, u))| < 0.18 */
    *col = hp_mix(h->hue, HP_WHITE, 0.6f * (1 - d) + canal);
    *a = 0.30f + 0.45f * d + canal;
    return true;
}
static bool abyss_core(float u, float v, const void *x, RGB *col, float *a) {
    const HueCtx *h = x;
    *col = hp_mix(h->hue, HP_WHITE, 0.7f);
    *a = 0.35f * (1 - (u * u + v * v));
    return true;
}
static bool flat_dark(float u, float v, const void *x, RGB *col, float *a) {
    (void)u; (void)v; (void)x;
    *col = (RGB){3, 8, 18};
    *a = 0.9f;
    return true;
}
/* DS: the god-rays are one cached stamp (their rest positions) whose brightness breathes, instead of seven rays
 * redrawn every frame (~3,300 blends) */
static void abyss_rays(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H;
    for (int k = 0; k < 7; k++) {                                        /* god-rays from the surface */
        float x0 = W * (-0.05f + k * 0.17f) + fsin(k * 1.7f) * 6, sw = 1.0f;
        for (int y = 0; y < (int)(H * 0.75f); y++) {
            float f = y / (H * 0.75f), xc = x0 + y * 0.45f, w = 2 + f * 8;
            int k0 = hp_k(0.13f * (1 - f) * sw), iw = (int)w, step = (int)(65536 / (w + 1)), xi = (int)floorf(xc);
            for (int dx = -iw; dx <= iw; dx++) hp_blendi(P, xi + dx, y, (RGB){110, 210, 230}, (k0 * (65536 - abs(dx) * step)) >> 16);
        }
    }
}
static void l_abyss(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    hp_stamp(P, 0, abyss_rays, NULL, 0.7f + 0.3f * fsin(t * 0.8f));

    for (int i = 0; i < 80; i++) {                                       /* plankton */
        float x = hp_mod(i * 97.3f + fsin(t * 0.4f + i) * 4, W), y = hp_mod(i * 53.1f - t * (1.5f + i % 4), H);
        float tw = 0.5f + 0.5f * fsin(t * 3 + i * 2.1f);
        hp_blend(P, x, y, i % 3 ? (RGB){120, 255, 230} : (RGB){200, 160, 255}, 0.25f + 0.6f * tw);
    }
    float S = fminf(1.4f, fmaxf(0.7f, H / 80));
    static const float JX[4] = {0.13f, 0.87f, 0.76f, 0.25f}, JS[4] = {1.0f, 1.15f, 0.55f, 0.45f}, JV[4] = {1.0f, 0.75f, 1.4f, 1.7f};
    static const RGB JH[4] = {{255, 130, 220}, {130, 220, 255}, {190, 255, 210}, {255, 210, 140}};
    for (int j = 0; j < 4; j++) {
        float span = H * 1.5f, sz = JS[j];
        float cy = H * 0.95f - hp_mod(t * JV[j] * 2.4f + j * span * 0.31f, span) + H * 0.1f, cx = W * JX[j] + fsin(t * 0.5f + j * 2) * 4;
        float pulse = 0.5f + 0.5f * fsin(t * 2.2f + j * 1.3f), rx = 12 * sz * S * (1.0f + 0.12f * pulse), ry = 9 * sz * S * (1.0f - 0.15f * pulse);
        RGB hue = JH[j], h25 = hp_mix(hue, HP_WHITE, 0.25f), h50 = hp_mix(hue, HP_WHITE, 0.5f);
        for (int k = 0; k < 10; k += 2) {                                /* long trailing tendrils (DS: every other one) */
            float tx = cx + (k - 4.5f) * rx * 0.19f;
            int ln = (int)(36 * sz * S);
            int yb = (int)(cy + ry * 0.15f);
            for (int s = 0; s < ln; s++) hp_blendi(P, hp_i(tx + fsin(s * 0.2f - t * 2.0f + k) * (0.6f + s * 0.07f)), yb + s, h25, 141 - 141 * s / ln);
        }
        for (int k = 0; k < 4; k++) {                                    /* frilled oral arms */
            float tx = cx + (k - 1.5f) * rx * 0.22f, n = 16 * sz * S;
            for (int s = 0; s < (int)n; s++) {
                float w = 1.6f * (1 - s / n), ax = tx + fsin(s * 0.45f + t * 2.6f + k) * 1.8f;
                int ka = hp_k(0.5f * (1 - s / n)), yi = hp_i(cy + 1 + s);
                for (int dw = -1; dw <= 1; dw++) hp_blendi(P, hp_i(ax + dw * w), yi, h50, ka);
            }
        }
        {                                                                /* the bell, in integers (u, v, d in Q8) */
            int32_t irx = (int32_t)(65536 / fmaxf(0.5f, rx)), iry = (int32_t)(65536 / fmaxf(0.5f, ry)), cx8 = (int32_t)(cx * 256), cy8 = (int32_t)(cy * 256);
            for (int y = (int)(cy - ry) - 1; y < (int)(cy + ry) + 2; y++) {
                int32_t v = (((y * 256 + 128) - cy8) * iry) >> 16;
                if (v > 51) continue;                                    /* v > 0.2: open below the rim */
                for (int x = (int)(cx - rx) - 1; x < (int)(cx + rx) + 2; x++) {
                    int32_t u = (((x * 256 + 128) - cx8) * irx) >> 16, d = (u * u + v * v) >> 8;
                    if (d > 256) continue;
                    int64_t s4 = (int64_t)4 * u * v * ((u * u - v * v) >> 8);   /* sin(4 theta) * d^2, Q24 */
                    int canal = (s4 < 0 ? -s4 : s4) < ((int64_t)46 * d * d) ? 64 : 0;    /* 0.18 d^2 ; canal 0.25 */
                    int km = ((154 * (256 - d)) >> 8) + canal;           /* 0.6 (1 - d) + canal */
                    int ka = 77 + ((115 * d) >> 8) + canal;              /* 0.30 + 0.45 d + canal */
                    hp_blendi(P, x, y, hp_mixk(hue, HP_WHITE, km > 256 ? 256 : km), ka);
                }
            }
            float qx = cx, qy = cy - ry * 0.25f, qrx = rx * 0.45f, qry = ry * 0.4f;   /* its glowing core */
            int32_t jrx = (int32_t)(65536 / fmaxf(0.5f, qrx)), jry = (int32_t)(65536 / fmaxf(0.5f, qry)), qx8 = (int32_t)(qx * 256), qy8 = (int32_t)(qy * 256);
            RGB core = hp_mix(hue, HP_WHITE, 0.7f);
            for (int y = (int)(qy - qry) - 1; y < (int)(qy + qry) + 2; y++) {
                int32_t v = (((y * 256 + 128) - qy8) * jry) >> 16;
                for (int x = (int)(qx - qrx) - 1; x < (int)(qx + qrx) + 2; x++) {
                    int32_t u = (((x * 256 + 128) - qx8) * jrx) >> 16, d = (u * u + v * v) >> 8;
                    if (d <= 256) hp_blendi(P, x, y, core, (90 * (256 - d)) >> 8);
                }
            }
        }
        for (int k = 0; k < 30; k++) {                                   /* glowing rim lights, chasing round the bell */
            float a = PI_F + k * PI_F / 29;
            hp_blend(P, cx + fcos(a) * rx, cy + fsin(a) * ry * 0.98f + 0.5f, HP_WHITE, 0.35f + 0.6f * fmaxf(0.0f, fsin(t * 4 - k * 0.5f)));
        }
        for (int k = 0; k < 16; k++) hp_blend(P, cx - rx + k * rx * 2 / 15, cy + ry * 0.2f, h50, 0.8f);
    }
    for (int b = 0; b < 5; b++) {                                        /* bubble columns */
        float bx = W * (0.04f + b * 0.23f);
        for (int i = 0; i < 9; i++) {
            float y = H - hp_mod(t * 9 + i * 7 + b * 13, H * 0.9f);
            hp_disc(P, bx + fsin(y * 0.3f + b) * 1.5f, y, 0.6f + (i % 3) * 0.3f, (RGB){200, 240, 255}, 0.5f, 0, HP_WHITE);
        }
    }
    float lx = W * 0.30f + fsin(t * 0.6f) * 3, ly = H * 0.60f + fsin(t * 1.1f) * 2;   /* the angler waiting in the dark */
    hp_ellipse_flat(P, lx + 10, ly + 9, 15, 7, (RGB){3, 8, 18}, 230);
    for (int k = 0; k < 8; k++) hp_blend(P, lx + 1 + k * 1.6f, ly + 8 + (k % 2), (RGB){220, 230, 240}, 0.75f);
    hp_disc(P, lx + 17, ly + 6, 1.0f, (RGB){255, 250, 210}, 0.9f, 0, HP_WHITE);
    for (int k = 0; k < 10; k++) {
        float f = k / 9.0f;
        hp_blend(P, lx + 8 - f * 8 + fsin(t * 1.3f) * f * f * 2, ly + 3 - f * 6, (RGB){40, 90, 110}, 0.85f);
    }
    hp_disc(P, lx + fsin(t * 1.3f) * 2, ly - 4, 1.5f, (RGB){210, 255, 245}, 0.7f + 0.3f * fsin(t * 3.1f), 6, (RGB){60, 210, 220});
    hp_commit(P);
}

/* ---- SPIRIT SHRINE ----------------------------------------------------------------------------------------- */
static void shrine_a(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H;
    float mx = W * 0.86f, my = H * 0.20f, mr = fminf(W * 0.11f, H * 0.17f);
    hp_disc(P, mx, my, mr, (RGB){255, 244, 222}, 1.0f, mr * 1.1f, (RGB){255, 214, 200});
    for (int k = 0; k < 6; k++) hp_disc(P, mx + fcos(k * 1.9f) * mr * 0.5f, my + fsin(k * 2.7f) * mr * 0.45f, mr * 0.15f, (RGB){236, 224, 204}, 0.7f, 0, HP_WHITE);
    static const float BASE[2] = {0.52f, 0.64f}, AMP[2] = {0.14f, 0.10f};
    static const RGB RC[2] = {{62, 42, 92}, {40, 26, 64}};
    for (int layer = 0; layer < 2; layer++) {                            /* far ranges */
        float pts[40][2];
        int n = 0;
        pts[n][0] = 0; pts[n][1] = H; n++;
        for (int x = 0; x < W + 4 && n < 38; x += 4) {
            pts[n][0] = x;
            pts[n][1] = H * BASE[layer] - AMP[layer] * H * (0.5f + 0.5f * fsin(x * 0.035f + layer * 2) * fcos(x * 0.013f + layer));
            n++;
        }
        pts[n][0] = W; pts[n][1] = H; n++;
        hp_poly(P, (const float (*)[2])pts, n, RC[layer], 1.0f);
    }
    float px = W * 0.86f, py = H * 0.36f;                                /* the shrine mountain on the right */
    const float mtn[6][2] = {{W * 0.62f, H}, {W * 0.74f, H * 0.55f}, {px - 6, py + 3}, {px + 6, py + 3}, {W * 1.02f, H * 0.5f}, {W * 1.02f, H}};
    hp_poly(P, mtn, 6, (RGB){30, 18, 46}, 1.0f);
    hp_quad(P, px - 9, py, px + 9, py, px + 6, py - 5, px - 6, py - 5, (RGB){160, 32, 30}, 1.0f);
    hp_quad(P, px - 6, py, px + 6, py, px + 6, py + 5, px - 6, py + 5, (RGB){60, 18, 22}, 1.0f);
    hp_disc(P, px, py + 2.5f, 1.3f, (RGB){255, 220, 140}, 1.0f, 4, (RGB){255, 170, 80});
    for (int k = 4; k >= 0; k--) {                                       /* the torii, far ones first */
        float gx = W * (0.64f + 0.046f * k), gy = H * (0.74f - 0.066f * k), s = 1.0f - 0.12f * k;
        RGB red = {226, 58, 40}, dark = {128, 26, 22}, blk = {28, 16, 18};
        float hw = 8 * s, hh = 15 * s;
        for (int side = -1; side <= 1; side += 2)
            for (int y = (int)(gy - hh); y <= (int)gy; y++) {
                hp_put(P, gx + side * hw * 0.62f, y, red);
                if (s > 0.5f) hp_put(P, gx + side * hw * 0.62f + 1, y, dark);
            }
        for (int x = (int)(gx - hw * 1.2f); x <= (int)(gx + hw * 1.2f); x++) {
            float q = (x - gx) / (hw * 1.2f), lift = q * q * q * q * 2.4f * s;
            hp_put(P, x, gy - hh - lift - 1, blk);
            hp_put(P, x, gy - hh - lift, red);
            if (fabsf(x - gx) <= hw * 0.85f) hp_put(P, x, gy - hh + 4 * s, red);
        }
        hp_put(P, gx, gy - hh + 2 * s, blk);
    }
    float lx = W * 0.14f, ly = H * 0.70f;                                /* a stone lantern with a living flame */
    hp_quad(P, lx - 6, ly, lx + 6, ly, lx + 4, ly - 3, lx - 4, ly - 3, (RGB){120, 116, 130}, 1.0f);
    hp_quad(P, lx - 2, ly - 3, lx + 2, ly - 3, lx + 2, ly - 12, lx - 2, ly - 12, (RGB){110, 106, 120}, 1.0f);
    hp_quad(P, lx - 5, ly - 12, lx + 5, ly - 12, lx + 5, ly - 18, lx - 5, ly - 18, (RGB){130, 126, 140}, 1.0f);
}
static void shrine_b(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H, lx = W * 0.14f, ly = H * 0.70f;
    hp_quad(P, lx - 8, ly - 18, lx + 8, ly - 18, lx + 2, ly - 23, lx - 2, ly - 23, (RGB){100, 96, 112}, 1.0f);
    const float BR[3][4] = {{0, H * 0.06f, 0.22f, W * 0.26f}, {W * 0.05f, H * 0.16f, -0.12f, W * 0.16f}, {W * 0.15f, H * 0.10f, 0.6f, W * 0.07f}};
    for (int b = 0; b < 3; b++)                                          /* cherry branches, heavy with blossom */
        for (int s = 0; s < (int)BR[b][3]; s++) {
            float x = BR[b][0] + s * fcos(BR[b][2]), y = BR[b][1] + s * fsin(BR[b][2]) + fsin(s * 0.08f + b) * 3;
            hp_put(P, x, y, (RGB){44, 24, 28});
            if (s < BR[b][3] * 0.5f) hp_put(P, x, y + 1, (RGB){44, 24, 28});
            if (s % 2 == 0)
                for (int k = 0; k < 3; k++)
                    hp_blend(P, x + fsin(s * 1.7f + k * 2 + b) * 3, y + fcos(s * 2.3f + k) * 2.6f - 1, (RGB){255, (uint8_t)(176 + (s * 7 % 50)), 210}, 0.9f);
        }
}
static void l_shrine(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time, lx = W * 0.14f, ly = H * 0.70f;
    hp_stamp(P, 0, shrine_a, NULL, 1);                                   /* moon, ranges, the shrine mountain, gates, lantern */
    hp_disc(P, lx, ly - 15, 1.8f, (RGB){255, 220, 150}, 0.75f + 0.25f * fsin(t * 9) * fsin(t * 4), 6, (RGB){255, 170, 80});
    hp_stamp(P, 1, shrine_b, NULL, 1);                                   /* lantern roof, cherry branches */
    for (int i = 0; i < 16; i++) {                                       /* foxfire wisps between the gates */
        float ph = t * 0.35f + i * 0.83f;
        float x = i % 2 ? W * (0.62f + hp_mod(i * 0.061f, 0.36f)) + fsin(ph * 2) * 4 : W * (0.03f + hp_mod(i * 0.043f, 0.26f)) + fsin(ph * 2) * 4;
        float y = H * (0.38f + 0.3f * hp_mod(i * 0.37f, 1)) + fsin(ph * 1.3f) * 3, fl = 0.6f + 0.4f * fsin(t * 7 + i * 2.3f);
        hp_disc(P, x, y, 0.8f, (RGB){232, 246, 255}, fl, 3, (RGB){80, 140, 255});
        hp_blend(P, x, y + 1.5f, (RGB){100, 160, 255}, 0.5f * fl);
    }
    for (int i = 0; i < 40; i++) {                                       /* petals on the wind */
        float x = hp_mod(i * 41.7f + t * (5 + i % 3) + fsin(t + i) * 6, W), y = hp_mod(i * 23.3f + t * (6 + i % 4), H);
        hp_blend(P, x, y, (RGB){255, 194, 218}, 0.85f);
        hp_blend(P, x + 1, y, (RGB){255, 150, 190}, 0.4f);
    }
    hp_commit(P);
}

/* ---- HOLO CITY --------------------------------------------------------------------------------------------- */
static void holo_city(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H, t = 0;
    PyRandom r;
    rng_seed(&r, 77);
    static const RGB DC[3] = {{44, 24, 70}, {26, 14, 46}, {12, 8, 24}}, DL[3] = {{120, 80, 160}, {255, 120, 220}, {90, 230, 255}};
    static const float HMIN[3] = {0.25f, 0.3f, 0.35f}, HMAX[3] = {0.5f, 0.65f, 0.8f};
    static const int WMIN[3] = {6, 8, 10}, WMAX[3] = {12, 16, 20};
    for (int depth = 0; depth < 3; depth++) {
        int x = -rng_randint(&r, 0, 6);
        while (x < W) {
            int bw = rng_randint(&r, WMIN[depth], WMAX[depth]);
            float bh = H * (float)rng_uniform(&r, HMIN[depth], HMAX[depth]), top = H - bh;
            hp_quad(P, x, H, x + bw, H, x + bw, top, x, top, DC[depth], 1.0f);
            if (rng_random(&r) < 0.4) {
                int ant = rng_randint(&r, 4, 10);
                hp_line(P, x + bw / 2, top, x + bw / 2, top - ant, DC[depth], 1.0f);
                if (((int)(t * 2) + x) % 3) hp_blend(P, x + bw / 2, top - 10, (RGB){255, 60, 80}, 1.0f);
            }
            for (int wy = (int)top + 3; wy < H - 2; wy += 3)
                for (int wx = x + 2; wx < x + bw - 1; wx += 3) {
                    int seed = ((wx * 31 + wy * 17 + depth) % 97 + 97) % 97;
                    if (seed % 3 == 0 && (seed + (int)(t * 0.5f + seed)) % 11) hp_blend(P, wx, wy, DL[depth], 0.35f + depth * 0.15f);
                }
            x += bw + rng_randint(&r, 1, 4);
        }
    }
}
static void l_holocity(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    hp_stamp(P, 0, holo_city, NULL, 1);                                  /* three depths of towers */
    const float BB[2][4] = {{W * 0.08f, H * 0.18f, 22, 26}, {W * 0.78f, H * 0.30f, 26, 18}};
    static const RGB B1[2] = {{90, 230, 255}, {255, 70, 210}}, B2[2] = {{255, 70, 210}, {255, 220, 90}};
    for (int j = 0; j < 2; j++) {                                        /* hologram billboards: scrolling glyph columns */
        float bx = BB[j][0], by = BB[j][1];
        int bw = (int)BB[j][2], bh = (int)BB[j][3];
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                int g = (x / 3 * 7 + (y + (int)(t * 8 * (1 + j))) / 3 * 13) % 9;
                bool on = g < 4 && x % 3 != 2 && y % 3 != 2;
                float scan = (y + (int)(t * 20)) % 5 == 0 ? 0.25f : 0.0f;
                if (on || scan > 0) hp_blend(P, bx + x, by + y, (x / 6 + j) % 2 ? B1[j] : B2[j], (on ? 0.55f : 0) + scan);
            }
        for (int x = 0; x < bw + 2; x++) { hp_blend(P, bx - 1 + x, by - 1, B1[j], 0.8f); hp_blend(P, bx - 1 + x, by + bh, B1[j], 0.8f); }
    }
    float kx = W * 0.5f + fsin(t * 0.35f) * W * 0.22f, ky = H * 0.22f + fsin(t * 0.7f) * 6;   /* the hologram koi */
    float head = fcos(t * 0.35f) >= 0 ? 1 : -1;
    for (int s = 0; s < 26; s++) {
        float f = s / 25.0f, x = kx - head * s * 1.2f, y = ky + fsin(t * 3 - s * 0.35f) * 2.5f * f;
        float w = 4.5f * fsin(fminf(1, f * 1.4f + 0.15f) * PI_F) + (f > 0.85f ? 3 * f : 0);
        for (int dy = -(int)w; dy <= (int)w; dy++) {
            int glitch = ((int)(t * 6) + s) % 23 == 0 ? 2 : 0;
            hp_blend(P, x + glitch, y + dy, (s / 4) % 2 ? (RGB){255, 120, 80} : (RGB){240, 250, 255}, fabsf((float)dy) < w - 1 ? 0.45f : 0.8f);
        }
    }
    for (int lane = 0; lane < 4; lane++) {                               /* flying traffic */
        float y = H * (0.25f + lane * 0.1f);
        for (int car = 0; car < 5; car++) {
            float spd = (14 + lane * 5) * (lane % 2 ? 1 : -1), x = hp_mod(car * W / 5 + t * spd, W + 20) - 10;
            RGB col = spd > 0 ? (RGB){255, 240, 220} : (RGB){255, 60, 60};
            for (int k = 0; k < 7; k++) hp_blend(P, x - (spd > 0 ? k : -k), y, col, 0.9f - k * 0.12f);
        }
    }
    for (int i = 0; i < 90; i++) {                                      /* rain */
        float x = hp_mod(i * 37.1f + t * 18, W), y = hp_mod(i * 61.7f + t * 70, H);
        hp_blend(P, x, y, (RGB){150, 170, 220}, 0.35f);
        hp_blend(P, x - 0.5f, y - 1, (RGB){150, 170, 220}, 0.25f);
    }
    hp_commit(P);
}

/* ---- THE TESSERACT ----------------------------------------------------------------------------------------- */
typedef struct { RGB c; } ColCtx;
static bool neb_fn(float u, float v, const void *x, RGB *col, float *a) { *col = ((const ColCtx *)x)->c; *a = 0.18f * (1 - (u * u + v * v)); return true; }
static void hyper_bg(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H;
    static const RGB NC[3] = {{90, 40, 170}, {40, 90, 190}, {170, 50, 140}};
    for (int k = 0; k < 3; k++) {                                        /* nebula clouds (DS: at rest) */
        ColCtx cc = {NC[k]};
        hp_ellipse(P, W * (0.2f + k * 0.33f) + fsin((float)k) * 6, H * (0.25f + 0.08f * k), W * 0.18f, H * 0.12f, neb_fn, &cc);
    }
    int hz = (int)(H * 0.58f);                                           /* synth grid floor */
    for (int i = -16; i <= 16; i++) hp_line(P, W / 2 + i * 3, hz, W / 2 + i * W * 0.09f, H, (RGB){170, 90, 255}, 0.35f);
}
static void l_hyper(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    int hz = (int)(H * 0.58f);
    hp_stamp(P, 0, hyper_bg, NULL, 1);
    for (int j = 1; j < 12; j++) {
        float q = (j + hp_mod(t * 0.6f, 1)) / 12, y = hz + (H - hz) * q * q;
        for (int x = 0; x < W; x++) hp_blend(P, x, y, (RGB){200, 110, 255}, 0.35f);
    }
    hp_line(P, 0, hz, W, hz, (RGB){255, 160, 255}, 0.7f);
    float cx = W * 0.84f, cy = H * 0.36f, S = fminf(W * 0.085f, H * 0.16f), a = t * 0.45f, b = t * 0.31f;
    float vx[16], vy[16], vw[16];
    for (int i = 0; i < 16; i++) {
        float x = (i & 1) ? 1 : -1, y = (i & 2) ? 1 : -1, z = (i & 4) ? 1 : -1, w = (i & 8) ? 1 : -1, n1, n2;
        n1 = x * fcos(a) - w * fsin(a); n2 = x * fsin(a) + w * fcos(a); x = n1; w = n2;                     /* rotate in XW */
        n1 = y * fcos(b) - z * fsin(b); n2 = y * fsin(b) + z * fcos(b); y = n1; z = n2;                     /* and YZ */
        n1 = z * fcos(a * 0.7f) - w * fsin(a * 0.7f); n2 = z * fsin(a * 0.7f) + w * fcos(a * 0.7f); z = n1; w = n2;
        float k4 = 2.2f / (3.2f - w), x3 = x * k4, y3 = y * k4, z3 = z * k4, k3 = 3.0f / (4.2f - z3);
        vx[i] = cx + x3 * k3 * S * 1.6f; vy[i] = cy + y3 * k3 * S; vw[i] = w;
    }
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++)
            if (__builtin_popcount((unsigned)(i ^ j)) == 1) {
                float f = hp_clamp((vw[i] + vw[j]) / 4 + 0.5f, 0, 1);
                hp_line(P, vx[i], vy[i], vx[j], vy[j], hp_mix((RGB){150, 80, 255}, (RGB){240, 240, 255}, f), 0.55f + 0.4f * f);
            }
    for (int i = 0; i < 16; i++) hp_disc(P, vx[i], vy[i], 0.9f, HP_WHITE, 0.9f, 2.5f, (RGB){190, 140, 255});
    for (int i = 0; i < 30; i++) {                                       /* motes falling into it */
        float ang = i * 2.39f + t * 0.2f, rr = (S * 2.4f) * (1 - hp_mod(t * 0.15f + i * 0.033f, 1));
        hp_blend(P, cx + fcos(ang) * rr * 1.6f, cy + fsin(ang) * rr, (RGB){220, 200, 255}, 0.7f);
    }
    hp_commit(P);
}

/* ---- KAIJU BAY --------------------------------------------------------------------------------------------- */
static bool cloud_fn(float u, float v, const void *x, RGB *col, float *a) { (void)u; (void)x; *col = (RGB){52, 58, 78}; *a = 0.55f * (1 - v * v); return true; }
static void kaiju_city(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H, t = 0;
    PyRandom r;
    rng_seed(&r, 9);
    int x = 0;
    float gnd = H * 0.74f;
    while (x < W * 0.30f) {                                              /* the city on the left shore */
        int bw = rng_randint(&r, 5, 10);
        float bh = H * (float)rng_uniform(&r, 0.14, 0.36);
        hp_quad(P, x, gnd, x + bw, gnd, x + bw, gnd - bh, x, gnd - bh, (RGB){14, 16, 26}, 1.0f);
        for (int wy = (int)(gnd - bh) + 2; wy < (int)gnd; wy += 3)
            for (int wx = x + 1; wx < x + bw - 1; wx += 2)
                if ((wx * 7 + wy * 3) % 5 == 0) hp_blend(P, wx, wy, (RGB){255, 210, 120}, 0.65f);
        if (rng_random(&r) < 0.5) hp_blend(P, x + bw / 2, gnd - bh - 2, (RGB){255, 60, 60}, (int)(t * 2 + x) % 2 ? 1.0f : 0.3f);
        x += bw + 1;
    }
}
typedef struct { float rear; bool bolt; } KaijuPose;
static void kaiju_body(HPx *P, const void *ctx) {
    const KaijuPose *ps = ctx;
    float W = P->W, H = P->H, gnd = H * 0.74f, rear = ps->rear;
    bool bolt = ps->bolt;
    float sc = fminf(1.1f, fmaxf(0.7f, H / 80)) * 0.85f, kx = W * 0.82f, ky = gnd + 2;
#define KX(dx) (kx - (dx) * sc)
#define KY(dy) (ky - (dy) * sc)
    const float body[12][2] = {{KX(-22), KY(0)}, {KX(-20), KY(22)}, {KX(-13), KY(36)}, {KX(-4), KY(44 + rear * 4)}, {KX(6), KY(48 + rear * 8)},
                               {KX(14), KY(46 + rear * 10)}, {KX(19), KY(40 + rear * 9)}, {KX(15), KY(35)}, {KX(10), KY(31)}, {KX(12), KY(23)},
                               {KX(22), KY(14)}, {KX(24), KY(0)}};
    RGB sil = bolt ? (RGB){2, 2, 4} : (RGB){6, 8, 10};
    hp_poly(P, body, 12, sil, 1.0f);
    hp_quad(P, KX(-20), KY(20), KX(-34), KY(18), KX(-40), KY(12), KX(-24), KY(12), sil, 1.0f);   /* tail sweeping behind */
    hp_quad(P, KX(10), KY(31), KX(19), KY(27), KX(21), KY(23), KX(12), KY(25), sil, 1.0f);       /* a clawed arm */
    for (int i = 0; i < 11; i++)                                         /* rim light from the plates and the storm */
        if (body[i + 1][1] < KY(10) || body[i][1] < KY(10))
            hp_line(P, body[i][0], body[i][1], body[i + 1][0], body[i + 1][1], bolt ? (RGB){230, 240, 255} : (RGB){90, 130, 180}, 0.55f);
#undef KX
#undef KY
}
static void l_kaiju(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    float cyc = hp_mod(t, 9.0f), gnd = H * 0.74f;
    bool bolt = ((int)(t * 10) % 61) <= 1;
    for (int i = 0; i < 6; i++) {                                        /* storm clouds rolling (integer ellipses) */
        float ex = hp_mod(i * W * 0.23f + t * 3, W * 1.3f) - W * 0.15f, ey = H * 0.08f + (i % 3) * 5, ax = W * 0.17f, ay = 8;
        int32_t iay = (int32_t)(65536 / ay), iax = (int32_t)(65536 / ax), ex8 = (int32_t)(ex * 256), ey8 = (int32_t)(ey * 256);
        for (int y = (int)(ey - ay) - 1; y < (int)(ey + ay) + 2; y++) {
            int32_t v = (((y * 256 + 128) - ey8) * iay) >> 16, v2 = (v * v) >> 8;
            if (v2 > 256) continue;
            int k = (141 * (256 - v2)) >> 8;                             /* 0.55 * (1 - v^2) */
            for (int x = (int)(ex - ax) - 1; x < (int)(ex + ax) + 2; x++) {
                int32_t u = (((x * 256 + 128) - ex8) * iax) >> 16;
                if (((u * u) >> 8) + v2 <= 256) hp_blendi(P, x, y, (RGB){52, 58, 78}, k);
            }
        }
    }
    hp_stamp(P, 0, kaiju_city, NULL, 1);                                 /* the city on the left shore */
    for (int k = 0; k < 2; k++) {                                        /* searchlights sweeping toward the monster */
        float bx = W * (0.08f + k * 0.14f), by = gnd - 2, ang = -PI_F / 2 + 0.55f + fsin(t * 0.5f + k * 2) * 0.45f, ca = fcos(ang), sa = fsin(ang);
        for (int s = 0; s < (int)(H * 0.8f); s++)
            for (int dw = -(int)(s * 0.07f); dw <= (int)(s * 0.07f); dw++) hp_blend(P, bx + ca * s - sa * dw, by + sa * s + ca * dw, (RGB){220, 230, 255}, 0.08f);
    }
    float sc = fminf(1.1f, fmaxf(0.7f, H / 80)) * 0.85f, kx = W * 0.82f, ky = gnd + 2;
#define KX(dx) (kx - (dx) * sc)
#define KY(dy) (ky - (dy) * sc)
    float rear = hp_clamp((cyc - 5.2f) / 0.8f, 0, 1) * (1 - hp_clamp((cyc - 8.0f) / 0.8f, 0, 1));
    KaijuPose pose = {rear, bolt};
    if (rear == 0 && !bolt) hp_stamp(P, 1, kaiju_body, &pose, 1);       /* at rest: the cached silhouette */
    else kaiju_body(P, &pose);
    float charge = hp_mod(t * 1.6f, 4.0f);
    for (int k = 0; k < 9; k++) {                                        /* dorsal plates, lit in a wave from tail to crown */
        float f = k / 8.0f, px = KX(-17 + f * 22), py = KY(21 + f * 25 + rear * 6 * f);
        float lit = fmaxf(0.0f, 1 - fabsf(charge - f * 3.2f) * 1.5f) + rear * 0.8f, h = (5 + 3 * fsin(f * PI_F)) * sc;
        hp_tri(P, px - 2 * sc, py, px + 2 * sc, py, px + 0.6f * sc, py - h, hp_mix((RGB){50, 74, 100}, (RGB){210, 244, 255}, fminf(1, lit)), 1.0f);
        if (lit > 0.3f) hp_disc(P, px, py - h * 0.5f, 1.5f, (RGB){120, 200, 255}, 0.45f * lit, 5, (RGB){60, 140, 255});
    }
    hp_disc(P, KX(13), KY(42 + rear * 9), 0.9f, (RGB){255, 140, 70}, 1.0f, 2.5f, (RGB){255, 70, 30});
    if (cyc > 6.0f && cyc < 7.6f) {                                      /* ATOMIC BREATH over the city */
        float mx = KX(19), my = KY(40 + rear * 9), grow = hp_clamp((cyc - 6.0f) / 0.35f, 0, 1), fade = 1 - hp_clamp((cyc - 7.25f) / 0.35f, 0, 1);
        for (int s = 0; s < (int)(W * 0.72f * grow); s++) {
            float y = my + s * 0.22f + fsin(s * 0.3f + t * 30) * 0.6f;
            for (int dw = -3; dw <= 3; dw++) hp_blend(P, mx - s, y + dw, hp_mix(HP_WHITE, (RGB){80, 160, 255}, abs(dw) / 3.0f), (0.95f - abs(dw) * 0.22f) * fade);
        }
        hp_disc(P, mx, my, 3, HP_WHITE, 0.95f * fade, 7, (RGB){120, 190, 255});
        for (int y = 0; y < (int)(H * 0.75f); y++)
            for (int xx = 0; xx < W; xx += 2) hp_blend(P, xx + (y % 2), y, (RGB){90, 150, 255}, 0.06f * fade);
    }
#undef KX
#undef KY
    for (int k = 0; k < 6; k++) {                                        /* ripples where it stands */
        float rx = (34 - k * 4 + hp_mod(t * 4, 4)) * sc;
        for (int j = 0; j < 48; j++) { float a = j * PI_F / 47; hp_blend(P, kx + fcos(a) * rx, ky - 1 + fsin(a) * 1.2f, (RGB){120, 150, 190}, 0.25f); }
    }
    for (int i = 0; i < 130; i++) {                                      /* driving rain */
        float rx = hp_mod(i * 29.3f - t * 30, W), ry = hp_mod(i * 71.9f + t * 80, H);
        hp_blend(P, rx, ry, (RGB){170, 180, 210}, 0.32f);
        hp_blend(P, rx + 0.4f, ry - 1, (RGB){170, 180, 210}, 0.2f);
    }
    if (bolt) {
        float bx = W * (0.4f + 0.5f * (((int)t * 37) % 10) / 10.0f), y = 0, bxx = bx;
        while (y < H * 0.6f) {
            float nx = bxx + ((((int)y * 13 + (int)t) % 7) - 3), ny = y + 4;
            hp_line(P, bxx, y, nx, ny, (RGB){240, 245, 255}, 1.0f);
            bxx = nx; y = ny;
        }
        for (int yy = 0; yy < (int)(H * 0.75f); yy++)
            for (int xx = 0; xx < W; xx += 2) hp_blend(P, xx + (yy % 2), yy, (RGB){200, 210, 255}, 0.18f);
    }
    hp_commit(P);
}

/* ---- GLOWSHROOM GROVE -------------------------------------------------------------------------------------- */
typedef struct { RGB c; float glow; } ShCtx;
static bool sh_aura(float u, float v, const void *x, RGB *col, float *a) { const ShCtx *s = x; *col = s->c; *a = 0.10f * s->glow * (1 - (u * u + v * v)); return true; }
static bool sh_cap(float u, float v, const void *x, RGB *col, float *a) {
    const ShCtx *s = x;
    if (v > 0.3f) return false;
    *col = hp_mix(s->c, HP_WHITE, 0.35f * (1 - (u * u + v * v)) * s->glow);
    *a = 1.0f;
    return true;
}
static const float SH_X[6] = {0.07f, 0.24f, 0.91f, 0.74f, 0.17f, 0.99f}, SH_Y[6] = {0.78f, 0.80f, 0.80f, 0.82f, 0.84f, 0.84f}, SH_S[6] = {1.15f, 0.7f, 1.25f, 0.65f, 0.4f, 0.55f};
static const RGB SH_C[6] = {{150, 60, 230}, {40, 200, 170}, {60, 170, 255}, {240, 80, 170}, {120, 240, 130}, {240, 80, 170}};
/* DS: the stems, auras and caps are one cached stamp (at rest, mid glow); gills, spots, spores and mist stay live */
static void shroom_bodies(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H;
    for (int i = 0; i < 6; i++) {
        float bx = W * SH_X[i], by = H * SH_Y[i], s = SH_S[i], h = 34 * s, sway = 0;
        for (int y = 0; y < (int)h; y++) {
            float f = y / h, w = (2.2f + 1.2f * (1 - f)) * s, cx = bx + sway * f * f;
            int iw = (int)w, step = (int)(256 / (w + 1)), xi = hp_i(cx), yi = hp_i(by - y);
            for (int dx = -iw; dx <= iw; dx++) hp_blendi(P, xi + dx, yi, hp_mixk((RGB){230, 220, 200}, (RGB){140, 130, 120}, abs(dx) * step), 256);
        }
        float top = by - h, glow = 0.85f, rx = 13 * s, ry = 7 * s;
        ShCtx sc = {SH_C[i], glow};
        hp_ellipse(P, bx + sway, top + ry * 0.2f, rx * 1.6f, ry * 1.8f, sh_aura, &sc);
        hp_ellipse(P, bx + sway, top, rx, ry, sh_cap, &sc);
    }
}
static void l_shrooms(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    hp_stamp(P, 0, shroom_bodies, NULL, 1);                              /* stems, auras, caps */
    for (int i = 0; i < 6; i++) {
        float bx = W * SH_X[i], by = H * SH_Y[i], s = SH_S[i], h = 34 * s;
        float top = by - h, glow = 0.75f + 0.25f * fsin(t * 1.3f + i * 1.7f), rx = 13 * s, ry = 7 * s;
        RGB g1 = hp_mix(SH_C[i], HP_WHITE, 0.5f), g2 = cshade(SH_C[i], 0.5f);
        int kg = hp_k(0.9f * glow);
        for (int k = 0; k < (int)(rx * 1.8f); k++) hp_blendi(P, hp_i(bx - rx * 0.9f + k), hp_i(top + ry * 0.3f + 1), k % 2 ? g1 : g2, kg);   /* gills */
        for (int k = 0; k < 7; k++) {                                    /* spots */
            float a = k * 2.4f + i;
            hp_disc(P, bx + fcos(a) * rx * 0.6f, top - ry * 0.45f + fsin(a * 1.3f) * ry * 0.3f, 1.1f * s, (RGB){255, 250, 230}, 0.8f * glow, 0, HP_WHITE);
        }
    }
    for (int i = 0; i < 50; i++) {                                       /* rising spores */
        float ph = hp_mod(t * 0.05f + i * 0.137f, 1.0f), x = W * hp_mod(i * 0.071f, 1) + fsin(t + i) * 4, y = H * (0.85f - ph * 0.8f);
        hp_blend(P, x, y, i % 2 ? (RGB){190, 255, 220} : (RGB){230, 190, 255}, 0.4f + 0.5f * fsin(t * 3 + i));
    }
    for (int k = 0; k < 3; k++) {                                        /* mist bands */
        float y0 = H * (0.66f + k * 0.05f);
        for (int x = 0; x < W; x++) {
            float o = fsin(x * 0.05f + t * 0.4f + k) * 2;
            hp_blendi(P, x, hp_i(y0 + o), (RGB){150, 200, 210}, 25);
            hp_blendi(P, x, hp_i(y0 + 1 + o), (RGB){150, 200, 210}, 17);
        }
    }
    hp_commit(P);
}

/* ---- THE DEEP DESERT --------------------------------------------------------------------------------------- */
static void worm_sky(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H, t = 0;
    static const float MX[3] = {0.18f, 0.30f, 0.86f}, MY[3] = {0.16f, 0.10f, 0.14f}, MR[3] = {7, 3.5f, 5};
    static const RGB MC[3] = {{255, 236, 200}, {220, 200, 255}, {255, 190, 150}};
    for (int i = 0; i < 3; i++) hp_disc(P, W * MX[i], H * MY[i], MR[i], MC[i], 1.0f, MR[i] * 1.2f, MC[i]);
    static const float DB[2] = {0.56f, 0.64f};
    static const RGB DCOL[2] = {{160, 104, 74}, {122, 76, 54}};
    for (int layer = 0; layer < 2; layer++) {                            /* far dunes */
        float pts[40][2];
        int n = 0;
        pts[n][0] = 0; pts[n][1] = H; n++;
        for (int x = 0; x < W + 3 && n < 38; x += 3) {
            pts[n][0] = x;
            pts[n][1] = H * DB[layer] - 5 * fsin(x * 0.04f + layer * 1.3f + t * 0.02f) - 3 * fsin(x * 0.11f + layer);
            n++;
        }
        pts[n][0] = W; pts[n][1] = H; n++;
        hp_poly(P, (const float (*)[2])pts, n, DCOL[layer], 1.0f);
    }
}
static void l_wormsea(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    hp_stamp(P, 0, worm_sky, NULL, 1);                                   /* three moons, far dunes */
    float cyc = hp_mod(t, 12.0f) / 12.0f;
    if (cyc > 0.08f && cyc < 0.75f) {                                    /* the breach */
        float p = (cyc - 0.08f) / 0.67f, x0 = W * 0.35f, x1 = W * 0.95f, ground = H * 0.66f, hgt = H * 0.52f;
        float px[70], py[70], pf[70];
        int np = 0;
        for (int k = 0; k < 70; k++) {
            float f = k / 69.0f, u = hp_clamp(p * 1.6f - (1 - f) * 0.9f, 0, 1);   /* the head leads, the body follows */
            if (u <= 0) continue;
            px[np] = x0 + (x1 - x0) * u; py[np] = ground - fsin(u * PI_F) * hgt; pf[np] = f; np++;
        }
        for (int i = 0; i < np; i++) {                                   /* body: thick ringed segments */
            float w = 5.5f + 2.5f * pf[i], ring = fsin(pf[i] * 50 - t * 4);
            for (int dy = -(int)w; dy <= (int)w; dy++) {
                float sh = 0.55f + 0.45f * (1 - fabsf((float)dy) / (w + 1));
                hp_blend(P, px[i], py[i] + dy, hp_mix((RGB){110, 72, 40}, (RGB){222, 180, 110}, (0.5f + 0.5f * ring) * sh), 1.0f);
            }
            if ((int)(pf[i] * 70) % 3 == 0)                              /* sand pouring off the rings */
                for (int s = 1; s < 10; s++) hp_blend(P, px[i] + fsin(s + t * 3) * 0.7f, py[i] + w + s * 1.4f, (RGB){226, 196, 140}, 0.5f * (1 - s / 10.0f));
        }
        if (np) {
            float hx = px[np - 1], hy = py[np - 1];
            hp_disc(P, hx, hy, 7.5f, (RGB){90, 50, 30}, 1.0f, 0, HP_WHITE);
            hp_disc(P, hx, hy, 5.5f, (RGB){255, 130, 50}, 1.0f, 4, (RGB){255, 90, 30});   /* the blazing maw */
            hp_disc(P, hx, hy, 2.5f, (RGB){255, 230, 170}, 1.0f, 0, HP_WHITE);
            for (int k = 0; k < 12; k++) {
                float a = k * TAU_F / 12 + t;
                hp_line(P, hx + fcos(a) * 5.5f, hy + fsin(a) * 5.5f, hx + fcos(a) * 7.8f, hy + fsin(a) * 7.8f, (RGB){250, 240, 220}, 0.9f);
            }
        }
        float xs[2] = {x0, x0 + (x1 - x0) * hp_clamp(p * 1.6f, 0, 1)};
        for (int k = 0; k < 30; k++)                                     /* sand fountains where it breaks the surface */
            for (int q = 0; q < 2; q++) hp_blend(P, xs[q] + fsin(k * 7.1f) * 6, ground - hp_mod(k * 1.7f + t * 9, 20), (RGB){230, 200, 150}, 0.4f);
    }
    for (int i = 0; i < 80; i++) {                                       /* blowing sand */
        float x = hp_mod(i * 43.3f + t * 26, W), y = H * 0.4f + hp_mod(i * 17.9f, H * 0.4f) + fsin(t + i) * 2;
        hp_blend(P, x, y, (RGB){240, 210, 160}, 0.35f);
        hp_blend(P, x - 1, y, (RGB){240, 210, 160}, 0.2f);
    }
    hp_commit(P);
}

/* ---- JADE TEMPLE ------------------------------------------------------------------------------------------- */
typedef struct { float x, y, f; bool front; } SerpPt;
static void jade_serpent(HPx *P, const SerpPt *serp, int n, bool front, float t) {
    for (int k = 0; k < n; k++) {
        float x = serp[k].x, y = serp[k].y, f = serp[k].f;
        if (serp[k].front != front) continue;
        float w = 3.2f * (1 - f * 0.65f);
        for (int dy = -(int)w; dy <= (int)w; dy++) {
            RGB c = ((k + dy) % 5 + 5) % 5 == 0 ? (RGB){250, 214, 120} : hp_mix((RGB){20, 140, 90}, (RGB){110, 230, 160}, 0.5f + 0.5f * fsin(k * 0.7f + dy));
            hp_blend(P, x, y + dy, front ? c : cshade(c, 0.6f), 1.0f);
        }
        if (k % 4 == 0 && k > 4 && k < n - 3) hp_line(P, x, y - w, x + 1.5f, y - w - 3.5f, chsv(0.38f + 0.08f * fsin((float)k), 0.8f, front ? 0.9f : 0.55f), 0.85f);
        if (k == 0) {
            hp_disc(P, x, y, 3.2f, (RGB){40, 170, 110}, 1.0f, 0, HP_WHITE);
            hp_blend(P, x - 1.5f, y - 1, (RGB){255, 60, 40}, 1.0f);
            for (int s = 0; s < 6; s++) hp_line(P, x + 1, y - 3, x + 2 + s, y - 8 - s * 0.6f, chsv(0.36f + s * 0.02f, 0.85f, 0.95f), 0.9f);   /* quetzal crest */
        }
        if (k == n - 1)
            for (int s = 0; s < 5; s++) hp_line(P, x, y, x + 9, y - 6 + s * 3 + fsin(t * 3 + s) * 2, s % 2 ? (RGB){230, 60, 50} : (RGB){60, 220, 150}, 0.85f);   /* tail plumes */
    }
}
static bool jungle_fn(float u, float v, const void *x, RGB *col, float *a) {
    int i = *(const int *)x;
    *col = hp_mix((RGB){18, 60, 36}, (RGB){70, 150, 80}, hp_clamp(-v * 0.8f + 0.2f + 0.15f * fsin(u * 9 + i), 0, 1));
    *a = 1.0f;
    return true;
}
/* DS: the sun disc (its slow turn frozen), the pyramid and the jungle are cached stamps; the serpent, flame, mist and
 * birds stay live */
#define JADE_DIMS float base = H * 0.74f, px = W * 0.84f, tw = fminf(W * 0.14f, H * 0.24f), th = fmaxf(3.0f, H * 0.055f); const int tiers = 7, n = 90; (void)n
static void jade_sun(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H, t = 0;
    float sx = W * 0.14f, sy = H * 0.27f, sr = fminf(W * 0.08f, H * 0.13f);
    for (int k = 0; k < 24; k++) {                                       /* sun rays */
        float a = k * TAU_F / 24 + t * 0.05f;
        hp_line(P, sx + fcos(a) * sr * 1.25f, sy + fsin(a) * sr * 1.25f, sx + fcos(a) * sr * 2.4f, sy + fsin(a) * sr * 2.4f, (RGB){255, 214, 120}, k % 2 ? 0.3f : 0.14f);
    }
    hp_disc(P, sx, sy, sr, (RGB){255, 220, 120}, 1.0f, sr * 0.8f, (RGB){255, 180, 80});
    hp_disc(P, sx, sy, sr * 0.62f, (RGB){240, 170, 70}, 0.6f, 0, HP_WHITE);
    for (int k = 0; k < 8; k++) {                                        /* carved glyph ring on the sun */
        float a = k * TAU_F / 8 + t * 0.1f;
        hp_disc(P, sx + fcos(a) * sr * 0.8f, sy + fsin(a) * sr * 0.8f, 1.0f, (RGB){170, 100, 40}, 0.9f, 0, HP_WHITE);
    }
    hp_disc(P, sx, sy, sr * 0.25f, (RGB){255, 240, 200}, 0.9f, 0, HP_WHITE);
}
static void jade_pyramid(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H;
    JADE_DIMS;
    for (int k = 0; k < tiers; k++) {
        float w = tw - k * tw / (tiers + 1.5f), y = base - k * th;
        hp_quad(P, px - w, y, px + w, y, px + w - 2, y - th, px - w + 2, y - th, hp_mix((RGB){58, 88, 66}, (RGB){116, 146, 104}, k / (float)tiers), 1.0f);
        hp_line(P, px - w + 2, y - th, px + w - 2, y - th, (RGB){160, 190, 140}, 0.85f);
        hp_line(P, px - w, y - 1, px + w, y - 1, (RGB){40, 60, 46}, 0.6f);
    }
    float top = base - tiers * th;
    hp_quad(P, px - 3, base, px + 3, base, px + 2, top, px - 2, top, (RGB){150, 130, 96}, 1.0f);   /* stairs */
    for (int y = (int)top; y < (int)base; y += 2) hp_line(P, px - 2.5f, y, px + 2.5f, y, (RGB){96, 80, 62}, 0.8f);
    hp_quad(P, px - 5, top, px + 5, top, px + 5, top - 7, px - 5, top - 7, (RGB){74, 104, 84}, 1.0f);   /* the temple */
    hp_tri(P, px - 6, top - 7, px + 6, top - 7, px, top - 11, (RGB){96, 130, 100}, 1.0f);
    hp_quad(P, px - 1.5f, top, px + 1.5f, top, px + 1.5f, top - 4, px - 1.5f, top - 4, (RGB){16, 20, 16}, 1.0f);
}
static void jade_jungle(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H;
    for (int side = 0; side < 2; side++) {                               /* jungle crowns with sunlit tops */
        float x0 = side ? W * 0.66f : 0;
        for (int i = 0; i < 12; i++) {
            float cx = side ? x0 + i * W * 0.03f : i * W * 0.028f;
            if (side && cx > W * 0.73f && cx < W * 0.95f) continue;
            float cy = H * (0.66f + 0.04f * fsin(i * 1.7f + side));
            float rx = 7 + (i * 5) % 4, ry = 5 + (i * 3) % 3;
            for (int x = (int)(cx - rx) - 1; x < (int)(cx + rx) + 2; x++) {
                float u = (x + 0.5f - cx) / rx, su = 0.15f * fsin(u * 9 + i);
                for (int y = (int)(cy - ry) - 1; y < (int)(cy + ry) + 2; y++) {
                    float v = (y + 0.5f - cy) / ry;
                    if (u * u + v * v <= 1.0f) hp_blendi(P, x, y, hp_mix((RGB){18, 60, 36}, (RGB){70, 150, 80}, hp_clamp(-v * 0.8f + 0.2f + su, 0, 1)), 256);
                }
            }
        }
    }
}
static void l_jadetemple(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    hp_stamp(P, 0, jade_sun, NULL, 1);
    JADE_DIMS;
    static SerpPt serp[90];
    for (int k = 0; k < n; k++) {                                        /* the serpent's coil: an ellipse around the pyramid */
        float f = k / (float)(n - 1), u = t * 0.55f - f * 2.6f, r = tw * 1.45f;
        serp[k].x = px + fcos(u) * r;
        serp[k].y = base - tiers * th * 0.62f + fsin(u) * r * 0.22f - f * 3 + fsin(k * 0.35f - t * 3) * 1.4f;
        serp[k].f = f;
        serp[k].front = fsin(u) >= 0;
    }
    jade_serpent(P, serp, n, false, t);
    hp_stamp(P, 1, jade_pyramid, NULL, 1);
    float top = base - tiers * th;
    hp_disc(P, px, top - 2, 0.8f, (RGB){255, 180, 80}, 0.7f + 0.3f * fsin(t * 8), 3, (RGB){255, 130, 40});
    jade_serpent(P, serp, n, true, t);
    hp_stamp(P, 2, jade_jungle, NULL, 1);
    for (int i = 0; i < 24; i++) hp_blend(P, hp_mod(i * 47.7f + t * 3, W), H * 0.55f + hp_mod(i * 13.3f, H * 0.15f), (RGB){230, 220, 200}, 0.18f);   /* mist */
    for (int b = 0; b < 3; b++) {                                        /* tiny birds */
        float bx = hp_mod(t * (6 + b) + b * 40, W + 20) - 10, by = H * (0.12f + b * 0.05f) + fsin(t * 2 + b) * 2;
        float wing = (int)(t * 6 + b) % 2 ? 1 : -1;
        hp_blend(P, bx, by, (RGB){30, 30, 40}, 1.0f);
        hp_blend(P, bx - 1, by + wing * 0.6f, (RGB){30, 30, 40}, 0.8f);
        hp_blend(P, bx + 1, by + wing * 0.6f, (RGB){30, 30, 40}, 0.8f);
    }
    hp_commit(P);
}


/* ---- THE ORRERY -------------------------------------------------------------------------------------------- */
static void orrery_stars(HPx *P, const void *ctx, bool lines, float t) {
    (void)ctx;
    float W = P->W, H = P->H;
    PyRandom r;
    rng_seed(&r, 12);
    for (int cn = 0; cn < 12; cn++) {                                    /* constellations, kept to the left half and the top */
        float cx = cn < 8 ? W * (0.04f + (cn % 4) * 0.08f) : W * (0.30f + (cn - 8) * 0.12f);
        float cy = cn < 8 ? H * (0.20f + (cn / 4) * 0.20f) : H * 0.06f;
        int np = rng_randint(&r, 4, 6);
        float pts[6][2];
        for (int i = 0; i < np; i++) { pts[i][0] = cx + (float)rng_uniform(&r, -7, 7); pts[i][1] = cy + (float)rng_uniform(&r, -5, 5); }
        if (lines) for (int i = 0; i + 1 < np; i++) hp_line(P, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], (RGB){200, 160, 80}, 0.22f);
        else for (int i = 0; i < np; i++) hp_blend(P, pts[i][0], pts[i][1], (RGB){255, 250, 220}, 0.5f + 0.5f * fsin(t * 2 + cn + i));
    }
}
static void orrery_lines(HPx *P, const void *ctx) { orrery_stars(P, ctx, true, 0); }
static const float ORR_R0[4] = {14, 22, 31, 40}, ORR_SPD[4] = {0.9f, -0.55f, 0.32f, -0.2f}, ORR_TILT[4] = {0.35f, 0.30f, 0.26f, 0.22f};
static const RGB ORR_BR[4] = {{230, 180, 90}, {210, 160, 70}, {190, 140, 60}, {170, 120, 50}}, ORR_PL[4] = {{150, 200, 255}, {255, 150, 100}, {200, 255, 210}, {255, 220, 160}};
static void orrery_rings(HPx *P, const void *ctx) {
    (void)ctx;
    float W = P->W, H = P->H, cx = W * 0.84f, cy = H * 0.36f, k_ = fminf(W * 0.15f / 60.0f, H * 0.30f / 40.0f);
    for (int q = 0; q < 4; q++) {
        float R = ORR_R0[q] * k_;
        int n = (int)(R * 9) + 12;
        for (int k = 0; k < n; k++) {
            float a = k * TAU_F / n, x = cx + fcos(a) * R * 1.5f, y = cy + fsin(a) * R * ORR_TILT[q] * 1.4f;
            hp_blend(P, x, y, ORR_BR[q], fsin(a) > 0 ? 0.9f : 0.45f);
            if (k % (n / 12 > 1 ? n / 12 : 1) == 0) hp_blend(P, x, y - 1, (RGB){255, 230, 150}, 0.75f);   /* tick marks */
        }
    }
    hp_disc(P, cx, cy, 5 * k_ + 1, (RGB){255, 220, 120}, 1.0f, 6 * k_ + 1, (RGB){255, 170, 60});   /* the sun */
}
static void l_orrery(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time;
    hp_stamp(P, 0, orrery_lines, NULL, 1);
    orrery_stars(P, NULL, false, t);
    float cx = W * 0.84f, cy = H * 0.36f, k_ = fminf(W * 0.15f / 60.0f, H * 0.30f / 40.0f);
    static const float R0[4] = {14, 22, 31, 40}, SPD[4] = {0.9f, -0.55f, 0.32f, -0.2f}, TILT[4] = {0.35f, 0.30f, 0.26f, 0.22f};
    static const RGB PL[4] = {{150, 200, 255}, {255, 150, 100}, {200, 255, 210}, {255, 220, 160}};
    for (int i = 0; i < 3; i++) {                                        /* meridian arcs, turning slowly */
        float R = (44 + i * 2) * k_, m = fcos(t * 0.15f + i * 1.1f);
        for (int k = 0; k < 160; k++) { float a = TAU_F * k / 159; hp_blend(P, cx + fcos(a) * R * 1.5f * m, cy + fsin(a) * R, (RGB){200, 150, 60}, 0.28f); }
    }
    hp_stamp(P, 1, orrery_rings, NULL, 1);                               /* brass rings and the sun */
    for (int k = 0; k < 8; k++) {
        float a = k * TAU_F / 8 + t * 0.4f;
        hp_line(P, cx + fcos(a) * (6 * k_ + 1.5f), cy + fsin(a) * (6 * k_ + 1.5f), cx + fcos(a) * (9 * k_ + 2), cy + fsin(a) * (9 * k_ + 2), (RGB){255, 200, 90}, 0.6f);
    }
    for (int q = 0; q < 4; q++) {                                        /* planets on brass arms */
        float R = R0[q] * k_, a = t * SPD[q] + R0[q], x = cx + fcos(a) * R * 1.5f, y = cy + fsin(a) * R * TILT[q] * 1.4f;
        hp_line(P, cx, cy, x, y, (RGB){150, 110, 50}, 0.4f);
        hp_disc(P, x, y, 1.2f + R0[q] * 0.035f, PL[q], 1.0f, 2.2f, PL[q]);
    }
    float cm = hp_mod(t * 0.07f, 1.0f), hx = W * (1.1f - cm * 1.3f), hy = H * (0.03f + cm * 0.12f);   /* a comet crossing the top */
    for (int s = 0; s < 30; s++) hp_blend(P, hx + s * 1.4f, hy - s * 0.2f, (RGB){200, 230, 255}, 0.8f * (1 - s / 30.0f));
    hp_disc(P, hx, hy, 1.2f, HP_WHITE, 1.0f, 2.5f, (RGB){160, 210, 255});
    hp_commit(P);
}

/* ---- GHOST ARMADA ------------------------------------------------------------------------------------------ */
static void l_armada(LCtx *c) {
    HPx *P = hp_begin(c);
    float W = P->W, H = P->H, t = c->time, mr = fminf(W * 0.09f, H * 0.14f);
    hp_disc(P, W * 0.86f, H * 0.2f, mr, (RGB){196, 255, 216}, 1.0f, mr, (RGB){90, 220, 150});
    for (int k = 0; k < 4; k++) {                                        /* cloud sea bands drifting */
        float y0 = H * (0.54f + k * 0.06f);
        RGB col = hp_mix((RGB){160, 200, 190}, (RGB){60, 90, 96}, k / 3.0f);
        for (int x = 0; x < W; x++) {
            float hgt = 3 + 2.5f * fsin(x * 0.07f + t * (0.3f + k * 0.1f) + k * 2) + 1.5f * fsin(x * 0.19f - t * 0.2f);
            int y = hp_i(y0 - hgt), n = (int)hgt + 6;
            for (int dy = 0; dy < n; dy++) hp_blendi(P, x, y + dy, col, dy < 2 ? 154 : 82);
        }
    }
    float sc = fminf(1.3f, fmaxf(0.7f, H / 80));
    static const float SX[3] = {0.07f, 0.80f, 0.16f}, SY[3] = {0.36f, 0.44f, 0.54f}, SSZ[3] = {0.42f, 0.62f, 1.0f}, SSP[3] = {0.010f, 0.006f, 0.004f};
    for (int i = 0; i < 3; i++) {
        float s = SSZ[i] * sc, x = (hp_mod(SX[i] + t * SSP[i], 1.25f) - 0.1f) * W;
        if (x > W * 0.30f && x < W * 0.70f && s > 0.9f) x = W * 0.16f + (x - W * 0.3f) * 0.1f;   /* the flagship keeps clear of the menu */
        float y = H * SY[i] + fsin(t * 0.9f + i) * 1.5f, L = 30 * s, hh = 6 * s;
        const float hull[6][2] = {{x - L * 0.5f, y - hh}, {x - L * 0.38f, y - hh * 1.6f}, {x + L * 0.5f, y - hh * 1.4f}, {x + L * 0.62f, y - hh * 0.6f}, {x + L * 0.4f, y}, {x - L * 0.36f, y}};
        hp_poly(P, hull, 6, (RGB){20, 36, 38}, 0.95f);
        for (int k = 0; k < 6; k++) hp_line(P, hull[k][0], hull[k][1], hull[(k + 1) % 6][0], hull[(k + 1) % 6][1], (RGB){110, 255, 170}, 0.45f);
        for (int k = 0; k < 5; k++) hp_disc(P, x - L * 0.28f + k * L * 0.15f, y - hh * 0.5f, 0.6f, (RGB){160, 255, 190}, 0.95f, 0, HP_WHITE);   /* gunports */
        static const float MM[3] = {-0.24f, 0.06f, 0.33f}, MH[3] = {0.9f, 1.2f, 0.85f};
        for (int m = 0; m < 3; m++) {                                    /* masts and tattered sails */
            float mx = x + MM[m] * L, mtop = y - hh - 26 * s * MH[m];
            hp_line(P, mx, y - hh, mx, mtop, (RGB){60, 84, 80}, 1.0f);
            for (int row = 0; row < 3; row++) {
                float sy0 = mtop + 2 * s + row * 8 * s * MH[m], sw = (6.5f - row * 1.2f) * s, sh = 6.5f * s * MH[m];
                float belly = (1.4f + 0.8f * fsin(t * 1.5f + MM[m] * 5 + row)) * s;
                hp_quad(P, mx - sw, sy0, mx + sw, sy0, mx + sw * 0.9f + belly, sy0 + sh, mx - sw * 0.9f + belly, sy0 + sh, (RGB){170, 255, 210}, 0.22f);
                hp_line(P, mx - sw, sy0, mx + sw, sy0, (RGB){200, 255, 225}, 0.6f);
                hp_line(P, mx - sw * 0.9f + belly, sy0 + sh, mx + sw * 0.9f + belly, sy0 + sh, (RGB){150, 240, 200}, 0.45f);
                for (int h = 0; h < 2; h++) {                            /* rips in the canvas */
                    float hx = mx + ((i * 7 + row * 3 + h * 5) % 9 - 4) * sw / 5, hy = sy0 + sh * (0.35f + 0.3f * h);
                    hp_disc(P, hx + belly * 0.5f, hy, 0.9f * s, (RGB){16, 30, 32}, 0.75f, 0, HP_WHITE);
                }
            }
        }
        hp_line(P, x + L * 0.5f, y - hh * 1.4f, x + L * 0.9f, y - hh * 2.4f, (RGB){60, 84, 80}, 0.9f);   /* bowsprit */
        hp_disc(P, x - L * 0.42f + fsin(t * 2 + i) * 0.8f, y - hh * 2.0f, 0.9f, (RGB){220, 255, 220}, 1.0f, 3.5f, (RGB){90, 255, 150});   /* stern lantern */
        int nw = (int)(34 * s);
        for (int k = 0; k < nw; k++) hp_blend(P, x - L * 0.4f - k * 1.2f, y + 0.5f + fsin(k * 0.4f + t * 3) * 0.6f, (RGB){100, 255, 160}, 0.45f * (1 - k / (34 * s)));
    }
    for (int i = 0; i < 24; i++)                                         /* fog wisps and ghost lights */
        hp_blend(P, hp_mod(i * 53.7f + t * 5, W), H * 0.3f + hp_mod(i * 19.3f, H * 0.3f), (RGB){170, 255, 200}, 0.3f + 0.3f * fsin(t * 2 + i));
    hp_commit(P);
}
