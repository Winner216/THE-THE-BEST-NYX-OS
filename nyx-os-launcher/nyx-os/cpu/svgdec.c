// cpu/svgdec.c — Minimal SVG rasterizer for Nyx OS
//
// Vector-to-raster conversion for simple SVG files.  Everything is
// done with fixed-point (16.16) integer math — no FPU, no libm, no
// memory allocation.  Only a small, icon-friendly subset is rendered:
//
//   Elements:  svg (root), g, rect, circle, ellipse, line,
//              polyline, polygon, path
//   Path cmds: M L H V C S Q T Z (and lowercase relative forms)
//   Attrs:     width, height, viewBox, fill, stroke, stroke-width,
//              fill-opacity, opacity, transform (translate/scale)
//   Colors:    #rgb, #rrggbb, rgb()/rgba(), named colors; "none"
//   Gradients: linearGradient + radialGradient fills/strokes
//              (url(#id), objectBoundingBox or userSpaceOnUse units,
//              up to 8 gradients x 8 stops, focal point supported);
//              rounded rect corners (rx/ry)
//   Filters:   feDropShadow (url(#id) — offset, stdDeviation blur,
//              flood-color/opacity), up to 4, on any shape
//   Groups:    <g> inherits paint + transform to its children
//
// Unsupported (ignored gracefully): pattern gradients, spreadMethod
// (pad/repeat/reflect), gradientTransforms, masks, filters other
// than feDropShadow, text, rotate()/matrix()/skew* transforms,
// <use>, CSS styling.  These are skipped without failing the decode.
//
// Output is 32-bit RGBA (non-premultiplied) into a caller-provided
// buffer of at least width*height*4 bytes.

#include <svgdec.h>
#include <stdint.h>
#include <stddef.h>

#define FX_SHIFT 16
#define FX_ONE   (1 << FX_SHIFT)

// ─── Fixed-point helpers ─────────────────────────────────────────
static int32_t fx_mul(int32_t a, int32_t b) {   // (a*b) >> 16
    return (int32_t)(((int64_t)a * b) >> FX_SHIFT);
}
static int32_t fx_div(int32_t a, int32_t b) {   // (a<<16) / b
    if (b == 0) return 0;
    return (int32_t)(((int64_t)a << FX_SHIFT) / b);
}
static int fx_to_int(int32_t v) { return v >> FX_SHIFT; }

// Integer square root (floor) of a non-negative 64-bit value —
// Newton's method, no FPU/libm.  Used by radial-gradient sampling.
static int64_t isqrt64(int64_t n) {
    if (n <= 0) return 0;
    int64_t x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

// ─── Parser helpers ──────────────────────────────────────────────
static int ci_eq(const char* a, const char* b, int len) { // case-insens
    for (int i = 0; i < len; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

// Parse a decimal number (no exponent) into fixed point.
static const char* svg_num(const char* p, const char* end, int32_t* out) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
    int neg = 0;
    if (p < end && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; }
    int64_t ip = 0;
    while (p < end && *p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); p++; }
    int64_t fp = 0, fdiv = 1;
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9' && fdiv < 100000) {
            fp = fp * 10 + (*p - '0');
            fdiv *= 10;
            p++;
        }
    }
    int64_t v = (ip << FX_SHIFT) + (fp << FX_SHIFT) / fdiv;
    *out = (int32_t)(neg ? -v : v);
    return p;
}

static int svg_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a color: "#rgb", "#rrggbb", "rgb(r,g,b)", "rgba(r,g,b,a)"
// (0-255 or percentage components), a named color, or "none".
// Returns 0 on failure (or "none" with alpha 0).  *alpha is set to
// 255 unless an explicit rgba() alpha is present.
static int svg_color(const char* s, int len, uint8_t* r, uint8_t* g, uint8_t* b,
                     uint8_t* alpha) {
    *alpha = 255;
    while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n')) { s++; len--; }
    if (len >= 4 && s[0] == '#') {
        if (len == 4) {  // #rgb
            int rv = svg_hex(s[1]), gv = svg_hex(s[2]), bv = svg_hex(s[3]);
            if (rv < 0 || gv < 0 || bv < 0) return 0;
            *r = (uint8_t)(rv * 17); *g = (uint8_t)(gv * 17); *b = (uint8_t)(bv * 17);
            return 1;
        }
        if (len == 7) {
            int rv = svg_hex(s[1]) * 16 + svg_hex(s[2]);
            int gv = svg_hex(s[3]) * 16 + svg_hex(s[4]);
            int bv = svg_hex(s[5]) * 16 + svg_hex(s[6]);
            if (rv < 0 || gv < 0 || bv < 0) return 0;
            *r = (uint8_t)rv; *g = (uint8_t)gv; *b = (uint8_t)bv;
            return 1;
        }
        return 0;
    }
    // rgb(r,g,b) / rgba(r,g,b,a) — 0-255 ints or percentages
    if (len >= 4 && (s[0]=='r'||s[0]=='R') && (s[1]=='g'||s[1]=='G') &&
        (s[2]=='b'||s[2]=='B') && s[3]=='(') {
        const char* p = s + 4;
        const char* end = s + len;
        int32_t comp[4];
        int nv = 0;
        int is_pct[4] = {0,0,0,0};
        while (p < end && nv < 4) {
            int32_t tmp;
            const char* r2 = svg_num(p, end, &tmp);
            if (r2 == p) { p++; continue; }
            p = r2;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p < end && *p == '%') { is_pct[nv] = 1; p++; }
            comp[nv++] = tmp;
            if (p < end && *p == ')') break;
        }
        if (nv >= 3) {
            for (int i = 0; i < 3; i++) {
                // Non-percent components are 0-255 integers; percent
                // components are on a 0-100 scale.
                int32_t v = is_pct[i]
                    ? (int32_t)(((int64_t)comp[i] * 255) / (100 * FX_ONE))
                    : fx_to_int(comp[i]);
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                if (i == 0) *r = (uint8_t)v;
                else if (i == 1) *g = (uint8_t)v;
                else *b = (uint8_t)v;
            }
            if (nv >= 4) {
                // Alpha is a 0..1 float (or percent).  fx_mul already
                // yields the final 0..255 integer — wrapping it in
                // fx_to_int would double-shift it to 0.
                int32_t a = is_pct[3]
                    ? (int32_t)(((int64_t)comp[3] * 255) / (100 * FX_ONE))
                    : (int32_t)fx_mul(comp[3], 255);
                if (a < 0) a = 0;
                if (a > 255) a = 255;
                *alpha = (uint8_t)a;
            }
            return 1;
        }
        return 0;
    }
    static const char* names[] = {"black","white","red","green","blue",
        "yellow","cyan","magenta","orange","purple","gray","grey",
        "pink","brown","silver","gold","navy","teal","maroon","olive"};
    static const uint8_t rgb[20][3] = {
        {0,0,0},{255,255,255},{255,0,0},{0,128,0},{0,0,255},
        {255,255,0},{0,255,255},{255,0,255},{255,165,0},{128,0,128},
        {128,128,128},{128,128,128},{255,192,203},{165,42,42},
        {192,192,192},{255,215,0},{0,0,128},{0,128,128},
        {128,0,0},{128,128,0}};
    for (int i = 0; i < 20; i++) {
        int nl = 0; while (names[i][nl]) nl++;
        if (len == nl && ci_eq(s, names[i], nl)) {
            *r = rgb[i][0]; *g = rgb[i][1]; *b = rgb[i][2];
            return 1;
        }
    }
    if (len == 4 && ci_eq(s, "none", 4)) return 0;   // "none" fill
    return 0;
}

// ─── Raster state ────────────────────────────────────────────────
typedef struct {
    uint8_t* pix;
    int      W, H;
    int32_t  vbx, vby, vbw, vbh;   // viewBox (fixed)
    int32_t  sx, sy;               // scale: viewBox unit → pixel (fixed)
    uint8_t* shbuf;                // non-NULL → feDropShadow capture mode
    int32_t  sh_dx, sh_dy;         // shadow offset (pixels)
    int      sh_blur;              // box-blur radius (pixels)
    uint8_t  sh_r, sh_g, sh_b;     // flood color
    int      sh_a;                 // flood alpha 0..255 (incl. flood-opacity)
} Raster;

static void rs_put(Raster* rs, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= rs->W || y >= rs->H) return;
    if (rs->shbuf) {
        // feDropShadow pass: record only the coverage silhouette
        // (max alpha wins), then return so nothing touches the real
        // framebuffer during capture.
        uint8_t* s = &rs->shbuf[y * rs->W + x];
        if (a > *s) *s = a;
        return;
    }
    uint8_t* p = rs->pix + (y * rs->W + x) * 4;
    if (a >= 255) {
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    } else if (a > 0) {
        // simple source-over blend
        p[0] = (uint8_t)((r * a + p[0] * (255 - a)) / 255);
        p[1] = (uint8_t)((g * a + p[1] * (255 - a)) / 255);
        p[2] = (uint8_t)((b * a + p[2] * (255 - a)) / 255);
        if (p[3] < 255) p[3] = 255;
    }
}

// Map a viewBox coordinate to a pixel coordinate (fixed point).
static int32_t rs_x(Raster* rs, int32_t vx) { return fx_mul(vx - rs->vbx, rs->sx); }
static int32_t rs_y(Raster* rs, int32_t vy) { return fx_mul(vy - rs->vby, rs->sy); }

// ─── Gradients (linear + radial) ─────────────────────────────────
#define SVG_MAX_GRADS 8
#define SVG_MAX_STOPS 8
typedef struct {
    int32_t off;                 // 0..65536
    uint8_t r, g, b, a;
} SvgStop;
typedef struct {
    char     id[16];
    int      radial;             // 1 = radialGradient, 0 = linear
    int      user_space;         // gradientUnits="userSpaceOnUse"
    int32_t  x1, y1, x2, y2;     // linear axis (bbox fractions or user units)
    int32_t  cx, cy, r;          // radial center + radius (same units)
    int32_t  fx, fy;             // radial focal point (defaults to cx,cy)
    int      nstops;
    SvgStop  stops[SVG_MAX_STOPS];
} SvgGrad;
typedef struct {
    uint8_t r, g, b, a;          // solid color + alpha
    const SvgGrad* grad;         // non-NULL → gradient paint
    int     radial;              // gradient is a radialGradient
    int32_t px1, py1, px2, py2;  // linear axis, fixed pixels
    int32_t pcx, pcy;            // radial center, fixed pixels
    int32_t prx, pry;            // radial radii, fixed pixels
    int32_t pfx, pfy;            // radial focal, fixed pixels
} SvgBrush;

static SvgGrad svg_grads[SVG_MAX_GRADS];
static int     svg_grad_n = 0;

// Register a gradient by id (reuses an existing slot with that id).
static int svg_grad_slot(const char* id, int idlen) {
    for (int i = 0; i < svg_grad_n; i++) {
        int n = 0; while (svg_grads[i].id[n]) n++;
        if (n == idlen) {
            int eq = 1;
            for (int k = 0; k < idlen; k++) if (svg_grads[i].id[k] != id[k]) { eq = 0; break; }
            if (eq) return i;
        }
    }
    if (svg_grad_n >= SVG_MAX_GRADS) return -1;
    int i = svg_grad_n++;
    int n = idlen < 15 ? idlen : 15;
    for (int k = 0; k < n; k++) svg_grads[i].id[k] = id[k];
    svg_grads[i].id[n] = 0;
    svg_grads[i].nstops = 0;
    svg_grads[i].radial = 0;
    svg_grads[i].user_space = 0;
    return i;
}

// Resolve "url(#id)" to a gradient slot, or -1.
static int svg_grad_find(const char* v, int vlen) {
    if (vlen < 5 || v[0] != 'u' || v[1] != 'r' || v[2] != 'l' || v[3] != '(' || v[4] != '#')
        return -1;
    int i = 5, idlen = 0;
    while (i + idlen < vlen && v[i + idlen] != ')') idlen++;
    for (int k = 0; k < svg_grad_n; k++) {
        int n = 0; while (svg_grads[k].id[n]) n++;
        if (n == idlen) {
            int eq = 1;
            for (int j = 0; j < idlen; j++) if (svg_grads[k].id[j] != v[i + j]) { eq = 0; break; }
            if (eq) return k;
        }
    }
    return -1;
}

// Gradient t (0..65536) for pixel (x,y).  Geometry is precomputed in
// pixel space by svg_grad_geom() — both gradientUnits modes reduce to
// the same pixel-coordinate math here.
//
// Linear:  t = projection of (px,py) onto the axis (px1,py1)-(px2,py2).
// Radial:  t = distance from the focal point to the pixel, normalized
//          by the ray's intersection with the ellipse edge (so the
//          focal point = start color, the edge = end color, per spec).
static int32_t grad_t_at(const SvgBrush* br, int x, int y) {
    int32_t px = ((int32_t)x << FX_SHIFT) + (FX_ONE >> 1);
    int32_t py = ((int32_t)y << FX_SHIFT) + (FX_ONE >> 1);
    if (!br->radial) {
        int32_t dx = br->px2 - br->px1, dy = br->py2 - br->py1;
        int64_t len2 = (int64_t)dx * dx + (int64_t)dy * dy;
        if (len2 == 0) return 0;
        int64_t t = ((int64_t)(px - br->px1) * dx +
                     (int64_t)(py - br->py1) * dy) / len2;
        if (t < 0) t = 0;
        if (t > FX_ONE) t = FX_ONE;
        return (int32_t)t;
    }
    // Radial.  First map the pixel into the ellipse's own space (the
    // center at origin, radii scaled to 1.0) so a circle test applies:
    //   X = (px - pcx)/prx, Y = (py - pcy)/pry
    // The focal point maps the same way: Fx, Fy.  The ray from the
    // focal point F through P hits the unit circle at E = F + s*(P-F)
    // with s = -(F·d) + sqrt((F·d)^2 - |F|^2 + 1), d = P - F.  Per
    // spec t = |P-F| / |E-F| = 1/s, i.e. t = 1 / (-(F·d) + sqrt(...)).
    // Map the pixel and focal point into the ellipse's own unit space
    // (center at origin, radii scaled to 1.0):
    //   X = (px - pcx)/prx, Y = (py - pcy)/pry, and Fx/Fy likewise.
    // The gradient runs along rays from the focal point F through the
    // pixel P; per spec t = |P-F| / |E-F| where E is the ray's first
    // hit on the unit circle.  With d = P - F and d̂ = d/|d|:
    //   t = |d| / (-(F·d̂) + sqrt((F·d̂)² - |F|² + 1))
    int32_t X = fx_div(px - br->pcx, br->prx);
    int32_t Y = fx_div(py - br->pcy, br->pry);
    int32_t Fx = fx_div(br->pfx - br->pcx, br->prx);
    int32_t Fy = fx_div(br->pfy - br->pcy, br->pry);
    int32_t dx = X - Fx, dy = Y - Fy;
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
    if (d2 == 0) return 0;                // pixel at the focal point
    int64_t dlen = isqrt64(d2);           // |d| (fixed)
    int32_t udx = fx_div(dx, (int32_t)dlen);
    int32_t udy = fx_div(dy, (int32_t)dlen);
    int64_t Fdot = (int64_t)fx_mul(Fx, udx) + fx_mul(Fy, udy);  // F·d̂
    int64_t F2 = (int64_t)Fx * Fx + (int64_t)Fy * Fy;           // |F|² (32.32)
    // (F·d̂)² - |F|² + 1, all normalized to 16.16.  Fdot² and F2 are
    // 32.32 products, so shift each back before combining — otherwise
    // the +1 lands at the wrong scale (works only for focal at center).
    int64_t disc = ((Fdot * Fdot) >> FX_SHIFT) - (F2 >> FX_SHIFT) + FX_ONE;
    if (disc < 0) disc = 0;
    // sqrt in 16.16: disc is value*2^16, so disc<<16 is value*2^32
    // and isqrt64 returns sqrt(value)*2^16 — exactly the 16.16 sqrt.
    int64_t s = isqrt64(disc << FX_SHIFT);
    int64_t den = s - Fdot;
    if (den <= 0) return FX_ONE;          // ray misses the ellipse
    int64_t t = (dlen * FX_ONE) / den;
    if (t < 0) t = 0;
    if (t > FX_ONE) t = FX_ONE;
    return (int32_t)t;
}

// Sample the gradient at t — linear interpolation between stops.
static void grad_color(const SvgGrad* g, int32_t t,
                       uint8_t* r, uint8_t* gg, uint8_t* b, uint8_t* a) {
    if (g->nstops <= 0) { *r = 0; *gg = 0; *b = 0; *a = 0; return; }
    // Per spec, everything before the first stop takes its color
    // (clamp — interpolating there would extrapolate backwards).
    if (t <= g->stops[0].off) {
        const SvgStop* s0 = &g->stops[0];
        *r = s0->r; *gg = s0->g; *b = s0->b; *a = s0->a;
        return;
    }
    for (int i = 1; i < g->nstops; i++) {
        if (t <= g->stops[i].off) {
            const SvgStop* s0 = &g->stops[i - 1];
            const SvgStop* s1 = &g->stops[i];
            int32_t span = s1->off - s0->off;
            int32_t f = span > 0 ? fx_div(t - s0->off, span) : 0;
            *r = (uint8_t)(s0->r + (int32_t)(((int64_t)(s1->r - s0->r) * f) >> FX_SHIFT));
            *gg = (uint8_t)(s0->g + (int32_t)(((int64_t)(s1->g - s0->g) * f) >> FX_SHIFT));
            *b = (uint8_t)(s0->b + (int32_t)(((int64_t)(s1->b - s0->b) * f) >> FX_SHIFT));
            *a = (uint8_t)(s0->a + (int32_t)(((int64_t)(s1->a - s0->a) * f) >> FX_SHIFT));
            return;
        }
    }
    const SvgStop* s = &g->stops[g->nstops - 1];
    *r = s->r; *gg = s->g; *b = s->b; *a = s->a;
}

// ─── Drop-shadow filter (feDropShadow) ───────────────────────────
// The only filter primitive worth rasterizing: a soft, offset,
// flood-colored copy of the element's silhouette laid down behind
// it.  Render the shape once in capture mode (rs_put writes the
// coverage alpha into svg_shadow_buf instead of the framebuffer),
// box-blur that silhouette, composite it at (dx,dy) with the flood
// color/opacity, then draw the element again normally on top.
#define SVG_MAX_FILTERS 4
typedef struct {
    char    id[16];
    int     has_shadow;
    int32_t dx, dy;            // offset, user units (fixed)
    int32_t std_dev;           // stdDeviation, user units (fixed)
    uint8_t fr, fg, fb;        // flood color
    int32_t flood_a;           // flood alpha 0..255 (incl. flood-opacity)
} SvgFilter;

static SvgFilter svg_filters[SVG_MAX_FILTERS];
static int       svg_filter_n = 0;

// Register a filter by id (reuses an existing slot with that id).
static int svg_filter_slot(const char* id, int idlen) {
    for (int i = 0; i < svg_filter_n; i++) {
        int n = 0; while (svg_filters[i].id[n]) n++;
        if (n == idlen) {
            int eq = 1;
            for (int k = 0; k < idlen; k++) if (svg_filters[i].id[k] != id[k]) { eq = 0; break; }
            if (eq) return i;
        }
    }
    if (svg_filter_n >= SVG_MAX_FILTERS) return -1;
    int i = svg_filter_n++;
    int n = idlen < 15 ? idlen : 15;
    for (int k = 0; k < n; k++) svg_filters[i].id[k] = id[k];
    svg_filters[i].id[n] = 0;
    svg_filters[i].has_shadow = 0;
    svg_filters[i].dx = 0; svg_filters[i].dy = 0;
    svg_filters[i].std_dev = 0;
    svg_filters[i].fr = 0; svg_filters[i].fg = 0; svg_filters[i].fb = 0;
    svg_filters[i].flood_a = 255;
    return i;
}

// Resolve "url(#id)" to a filter slot, or -1.
static int svg_filter_find(const char* v, int vlen) {
    if (vlen < 5 || v[0] != 'u' || v[1] != 'r' || v[2] != 'l' || v[3] != '(' || v[4] != '#')
        return -1;
    int i = 5, idlen = 0;
    while (i + idlen < vlen && v[i + idlen] != ')') idlen++;
    for (int k = 0; k < svg_filter_n; k++) {
        int n = 0; while (svg_filters[k].id[n]) n++;
        if (n == idlen) {
            int eq = 1;
            for (int j = 0; j < idlen; j++) if (svg_filters[k].id[j] != v[i + j]) { eq = 0; break; }
            if (eq) return k;
        }
    }
    return -1;
}

// Silhouette scratch — one byte per canvas pixel.  The decode cap is
// W*H <= 1M pixels (svg_decode rejects larger), so this always fits.
static uint8_t svg_shadow_buf[1024 * 1024];

static void rs_shadow_begin(Raster* rs, const SvgFilter* fl) {
    for (int i = 0; i < rs->W * rs->H; i++) svg_shadow_buf[i] = 0;
    rs->shbuf = svg_shadow_buf;
    rs->sh_dx = fx_to_int(fx_mul(fl->dx, rs->sx));
    rs->sh_dy = fx_to_int(fx_mul(fl->dy, rs->sy));
    int32_t sd = fx_mul(fl->std_dev, (rs->sx + rs->sy) / 2);
    rs->sh_blur = fx_to_int(sd);
    if (rs->sh_blur < 1) rs->sh_blur = 1;
    if (rs->sh_blur > 48) rs->sh_blur = 48;
    rs->sh_r = fl->fr; rs->sh_g = fl->fg; rs->sh_b = fl->fb;
    rs->sh_a = fl->flood_a;
}

// Separable box blur (2 passes each way — soft enough for a shadow).
static void rs_shadow_blur(Raster* rs) {
    int W = rs->W, H = rs->H, r = rs->sh_blur;
    uint8_t* b = rs->shbuf;
    static int32_t tmp[4096];
    for (int iter = 0; iter < 2; iter++) {
        for (int y = 0; y < H; y++) {                 // horizontal
            uint8_t* row = b + y * W;
            int32_t sum = 0;
            for (int x = -r; x <= r; x++) sum += row[x < 0 ? 0 : (x >= W ? W - 1 : x)];
            for (int x = 0; x < W; x++) {
                tmp[x] = sum;
                int xin = x + r + 1, xout = x - r;
                if (xin >= W) xin = W - 1;
                if (xout < 0) xout = 0;
                sum += row[xin] - row[xout];
            }
            int32_t n = 2 * r + 1;
            for (int x = 0; x < W; x++) row[x] = (uint8_t)(tmp[x] / n);
        }
        for (int x = 0; x < W; x++) {                 // vertical
            int32_t sum = 0;
            for (int y = -r; y <= r; y++) sum += b[(y < 0 ? 0 : (y >= H ? H - 1 : y)) * W + x];
            for (int y = 0; y < H; y++) {
                tmp[y] = sum;
                int yin = y + r + 1, yout = y - r;
                if (yin >= H) yin = H - 1;
                if (yout < 0) yout = 0;
                sum += b[yin * W + x] - b[yout * W + x];
            }
            int32_t n = 2 * r + 1;
            for (int y = 0; y < H; y++) b[y * W + x] = (uint8_t)(tmp[y] / n);
        }
    }
}

// Composite the blurred silhouette as a shadow, then exit capture
// mode so subsequent rs_put calls paint the element itself.
static void rs_shadow_end(Raster* rs) {
    if (!rs->shbuf) return;
    rs_shadow_blur(rs);
    int W = rs->W, H = rs->H, dx = rs->sh_dx, dy = rs->sh_dy;
    uint8_t fr = rs->sh_r, fg = rs->sh_g, fb = rs->sh_b;
    int fa = rs->sh_a;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int a = rs->shbuf[y * W + x];
            if (!a) continue;
            a = a * fa / 255;
            int ox = x + dx, oy = y + dy;
            if (ox < 0 || oy < 0 || ox >= W || oy >= H) continue;
            uint8_t* p = rs->pix + (oy * W + ox) * 4;
            p[0] = (uint8_t)((fr * a + p[0] * (255 - a)) / 255);
            p[1] = (uint8_t)((fg * a + p[1] * (255 - a)) / 255);
            p[2] = (uint8_t)((fb * a + p[2] * (255 - a)) / 255);
            if (p[3] < 255) p[3] = 255;
        }
    }
    rs->shbuf = NULL;
}

// Paint one pixel through a brush (solid or gradient).
static void rs_put_brush(Raster* rs, int x, int y, const SvgBrush* br) {
    if (br->grad) {
        uint8_t r, g, b, a;
        grad_color(br->grad, grad_t_at(br, x, y), &r, &g, &b, &a);
        a = (uint8_t)(((int)a * br->a) / 255);
        rs_put(rs, x, y, r, g, b, a);
    } else {
        rs_put(rs, x, y, br->r, br->g, br->b, br->a);
    }
}

// ─── Polygon fill (scanline, even-odd) ───────────────────────────
static void rs_fill_poly(Raster* rs, const int32_t* xs, const int32_t* ys,
                         int n, const SvgBrush* br) {
    if (n < 3) return;
    // Y bounds in pixels
    int ymin = fx_to_int(ys[0]), ymax = ymin;
    for (int i = 1; i < n; i++) {
        int y = fx_to_int(ys[i]);
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    if (ymin < 0) ymin = 0;
    if (ymax >= rs->H) ymax = rs->H - 1;
    if (ymin > ymax) return;

    static int32_t xsect[512];
    for (int y = ymin; y <= ymax; y++) {
        int32_t yc = (y << FX_SHIFT) + (FX_ONE >> 1);   // pixel center
        int cnt = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            int32_t y0 = ys[j], y1 = ys[i];
            if (y0 == y1) continue;
            if ((yc < y0) == (yc < y1)) continue;        // not spanning
            int32_t t = fx_div(yc - y0, y1 - y0);
            int32_t x = xs[j] + fx_mul(xs[i] - xs[j], t);
            if (cnt < 512) xsect[cnt++] = x;
        }
        // Sort intersections (insertion — tiny counts)
        for (int i = 1; i < cnt; i++) {
            int32_t v = xsect[i];
            int k = i - 1;
            while (k >= 0 && xsect[k] > v) { xsect[k+1] = xsect[k]; k--; }
            xsect[k+1] = v;
        }
        for (int i = 0; i + 1 < cnt; i += 2) {
            int x0 = fx_to_int(xsect[i]), x1 = fx_to_int(xsect[i+1]);
            if (x0 < 0) x0 = 0;
            if (x1 >= rs->W) x1 = rs->W - 1;
            for (int x = x0; x <= x1; x++) rs_put_brush(rs, x, y, br);
        }
    }
}

// ─── Thick line stroke (capsule) ─────────────────────────────────
static void rs_stroke_line(Raster* rs, int32_t x0, int32_t y0,
                           int32_t x1, int32_t y1, int32_t halfw,
                           const SvgBrush* br) {
    if (halfw < FX_ONE / 8) halfw = FX_ONE / 8;
    int xmin = fx_to_int((x0 < x1 ? x0 : x1)) - fx_to_int(halfw) - 1;
    int xmax = fx_to_int((x0 < x1 ? x1 : x0)) + fx_to_int(halfw) + 1;
    int ymin = fx_to_int((y0 < y1 ? y0 : y1)) - fx_to_int(halfw) - 1;
    int ymax = fx_to_int((y0 < y1 ? y1 : y0)) + fx_to_int(halfw) + 1;
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax >= rs->W) xmax = rs->W - 1;
    if (ymax >= rs->H) ymax = rs->H - 1;

    int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
    int64_t len2 = dx*dx + dy*dy;
    int64_t hw2 = (int64_t)halfw * halfw;
    if (len2 == 0) len2 = 1;
    for (int y = ymin; y <= ymax; y++) {
        for (int x = xmin; x <= xmax; x++) {
            // Pixel center, absolute, and relative to the line start.
            int64_t px = ((int64_t)x << FX_SHIFT) + (FX_ONE >> 1);
            int64_t py = ((int64_t)y << FX_SHIFT) + (FX_ONE >> 1);
            int64_t wx = px - x0, wy = py - y0;   // w = P − A
            // Projection parameter along the segment as a fixed
            // 0..FX_ONE fraction: t = (w·v) / |v|².  (w·v) is a
            // plain ratio in [0,1] — treating it as 16.16 in a
            // multiply would pin the sample to the line start and
            // strokes would vanish.  Both operands are pre-scaled
            // by the same power of two (chosen per-pixel) so the
            // fixed product stays inside int64 even for long
            // strokes on large canvases.
            int64_t num = wx*dx + wy*dy;
            int64_t tf;
            if (num <= 0)        tf = 0;
            else if (num >= len2) tf = FX_ONE;
            else {
                int64_t a = num, b = len2;
                while (a >= (int64_t)1 << 40) { a >>= 1; b >>= 1; }
                tf = (a * FX_ONE) / b;
            }
            // Closest point on the segment (absolute), both terms
            // in the same frame so the distance test below is valid.
            int64_t cx = x0 + ((dx * tf) >> FX_SHIFT);
            int64_t cy = y0 + ((dy * tf) >> FX_SHIFT);
            int64_t ddx = px - cx, ddy = py - cy;
            if (ddx*ddx + ddy*ddy <= hw2)
                rs_put_brush(rs, x, y, br);
        }
    }
}

// ─── Circle / ellipse fill ───────────────────────────────────────
static void rs_fill_ellipse(Raster* rs, int32_t cx, int32_t cy,
                            int32_t rx, int32_t ry,
                            const SvgBrush* br) {
    if (rx <= 0 || ry <= 0) return;
    int xmin = fx_to_int(cx - rx), xmax = fx_to_int(cx + rx);
    int ymin = fx_to_int(cy - ry), ymax = fx_to_int(cy + ry);
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax >= rs->W) xmax = rs->W - 1;
    if (ymax >= rs->H) ymax = rs->H - 1;
    for (int y = ymin; y <= ymax; y++) {
        int32_t dy = (int32_t)(((int64_t)y << FX_SHIFT) + (FX_ONE >> 1) - cy);
        for (int x = xmin; x <= xmax; x++) {
            int32_t dx = (int32_t)(((int64_t)x << FX_SHIFT) + (FX_ONE >> 1) - cx);
            // Normalized test: (dx/rx)^2 + (dy/ry)^2 <= 1.  Dividing
            // first keeps every intermediate <= 2^33 — the raw
            // dx^2*ry^2 form overflows int64 even for small radii.
            int32_t dxn = fx_div(dx, rx);
            int32_t dyn = fx_div(dy, ry);
            int64_t d2 = (int64_t)dxn * dxn + (int64_t)dyn * dyn;
            if (d2 <= (int64_t)FX_ONE * FX_ONE)
                rs_put_brush(rs, x, y, br);
        }
    }
}

// Per-pixel rounded-rect inside test (fixed point, no sqrt — the
// corners use the same normalized ellipse test as rs_fill_ellipse).
static int in_rrect(int x, int y, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    int32_t rx, int32_t ry) {
    int32_t pcx = ((int32_t)x << FX_SHIFT) + (FX_ONE >> 1);
    int32_t pcy = ((int32_t)y << FX_SHIFT) + (FX_ONE >> 1);
    if (pcx < x0 || pcx > x1 || pcy < y0 || pcy > y1) return 0;
    int32_t dx = 0, dy = 0;
    if (pcx < x0 + rx) dx = x0 + rx - pcx;
    else if (pcx > x1 - rx) dx = pcx - (x1 - rx);
    if (pcy < y0 + ry) dy = y0 + ry - pcy;
    else if (pcy > y1 - ry) dy = pcy - (y1 - ry);
    if (dx <= 0 || dy <= 0) return 1;
    int32_t dxn = fx_div(dx, rx), dyn = fx_div(dy, ry);
    return (int64_t)dxn * dxn + (int64_t)dyn * dyn <= (int64_t)FX_ONE * FX_ONE;
}

static void rs_fill_rrect(Raster* rs, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          int32_t rx, int32_t ry, const SvgBrush* br) {
    int xmin = fx_to_int(x0), xmax = fx_to_int(x1);
    int ymin = fx_to_int(y0), ymax = fx_to_int(y1);
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax >= rs->W) xmax = rs->W - 1;
    if (ymax >= rs->H) ymax = rs->H - 1;
    for (int y = ymin; y <= ymax; y++)
        for (int x = xmin; x <= xmax; x++)
            if (in_rrect(x, y, x0, y0, x1, y1, rx, ry))
                rs_put_brush(rs, x, y, br);
}

// Stroke a rounded rect: 4 straight edges + 4 quarter-circle corner
// arcs (22.5° steps via the shared 32-point cosine table).  rx=0
// degrades to a plain rect outline.
static void rs_stroke_rrect(Raster* rs, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int32_t rx, int32_t ry, int32_t hw, const SvgBrush* br) {
    static const int32_t cos32[32] = {
        65536, 64277, 60547, 54491, 46341, 36410, 25080, 12785,
        0, -12785, -25080, -36410, -46341, -54491, -60547, -64277,
        -65536, -64277, -60547, -54491, -46341, -36410, -25080, -12785,
        0, 12785, 25080, 36410, 46341, 54491, 60547, 64277 };
    rs_stroke_line(rs, x0 + rx, y0, x1 - rx, y0, hw, br);
    rs_stroke_line(rs, x1, y0 + ry, x1, y1 - ry, hw, br);
    rs_stroke_line(rs, x1 - rx, y1, x0 + rx, y1, hw, br);
    rs_stroke_line(rs, x0, y1 - ry, x0, y0 + ry, hw, br);
    for (int q = 0; q < 4; q++) {
        int32_t ccx, ccy;
        int sx, sy;
        if (q == 0)      { ccx = x1 - rx; ccy = y0 + ry; sx = 1;  sy = -1; }
        else if (q == 1) { ccx = x1 - rx; ccy = y1 - ry; sx = 1;  sy = 1; }
        else if (q == 2) { ccx = x0 + rx; ccy = y1 - ry; sx = -1; sy = 1; }
        else             { ccx = x0 + rx; ccy = y0 + ry; sx = -1; sy = -1; }
        int32_t px = ccx + sx * fx_mul(rx, cos32[q % 2 ? 0 : 8]);
        int32_t py = ccy + sy * fx_mul(ry, cos32[q % 2 ? 8 : 0]);
        for (int k = 2; k <= 8; k += 2) {
            int32_t ca = cos32[k], cb = cos32[8 - k];
            int32_t nx, ny;
            if (q % 2 == 0) { nx = ccx + sx * fx_mul(rx, cb); ny = ccy + sy * fx_mul(ry, ca); }
            else            { nx = ccx + sx * fx_mul(rx, ca); ny = ccy + sy * fx_mul(ry, cb); }
            rs_stroke_line(rs, px, py, nx, ny, hw, br);
            px = nx; py = ny;
        }
    }
}

// ─── Attribute extraction ────────────────────────────────────────
static const char* svg_attr(const char* tag, int tlen,
                            const char* name, int* vlen) {
    const char* p = tag;
    const char* end = tag + tlen;
    int nlen = 0; while (name[nlen]) nlen++;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        if (p >= end) break;
        const char* ns = p;
        while (p < end && *p != '=' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '/' && *p != '>') p++;
        if (p - ns == nlen && ci_eq(ns, name, nlen)) {
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p < end && *p == '=') {
                p++;
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                char q = 0;
                if (p < end && (*p == '"' || *p == '\'')) { q = *p; p++; }
                const char* vs = p;
                while (p < end && (q ? *p != q : (*p != ' ' && *p != '\t' && *p != '>'))) p++;
                *vlen = (int)(p - vs);
                return vs;
            }
        }
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '>') p++;
    }
    return NULL;
}

static int svg_attr_num(const char* tag, int tlen, const char* name, int32_t* out) {
    int vlen = 0;
    const char* v = svg_attr(tag, tlen, name, &vlen);
    if (!v) return 0;
    svg_num(v, v + vlen, out);
    return 1;
}

// Like svg_attr_num, but converts percentages to 0..1 fractions:
// "50%" → 0.5 (fixed), "0.5" → 0.5 (fixed, unchanged).
static int svg_attr_pct(const char* tag, int tlen, const char* name, int32_t* out) {
    int vlen = 0;
    const char* v = svg_attr(tag, tlen, name, &vlen);
    if (!v) return 0;
    const char* p = v;
    const char* end = v + vlen;
    int32_t num;
    p = svg_num(p, end, &num);
    if (p == v) { *out = 0; return 0; }
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p < end && *p == '%') num /= 100;
    *out = num;
    return 1;
}

// ─── Paint + transform state (for <g> inheritance) ──────────────
// SVG paint attributes are inherited: a <g> sets values that all
// descendants see unless they override them.  svg_decode() keeps a
// frame stack of (paint, matrix) pairs — push on <g>, pop on </g>.
typedef struct {
    uint8_t fr, fg, fb;          // fill color
    int     has_fill;            // fill present (solid or gradient)
    int     fill_none;           // fill="none" explicitly set
    int     fill_grad;           // gradient slot, or -1
    int32_t fill_alpha;          // 0..255 effective fill alpha
    uint8_t sr, sg, sb;          // stroke color
    int     has_stroke;
    int     stroke_none;
    int     stroke_grad;         // gradient slot, or -1
    int32_t stroke_alpha;        // 0..255 effective stroke alpha
    int32_t stroke_w;            // stroke width (user units, fixed)
    int32_t opacity;             // 0..255 element opacity
    int     filter;              // feDropShadow slot, or -1
} SvgPaint;

typedef struct {
    int32_t a, b, c, d, e, f;    // 2x3 affine (fixed 16.16)
} SvgMtx;

static void svg_paint_default(SvgPaint* pt) {
    pt->fr = 0; pt->fg = 0; pt->fb = 0;
    pt->has_fill = 1; pt->fill_none = 0; pt->fill_alpha = 255;
    pt->fill_grad = -1;
    pt->sr = 0; pt->sg = 0; pt->sb = 0;
    pt->has_stroke = 0; pt->stroke_none = 0; pt->stroke_alpha = 255;
    pt->stroke_grad = -1;
    pt->stroke_w = FX_ONE;      // SVG default: 1 user unit
    pt->opacity = 255;
    pt->filter = -1;
}

static void svg_mtx_identity(SvgMtx* m) {
    m->a = FX_ONE; m->b = 0; m->c = 0;
    m->d = FX_ONE; m->e = 0; m->f = 0;
}

// Apply an element's paint attributes on top of inherited values.
// rgb()/rgba() alphas and fill-opacity/opacity multiply in, so a
// child can still override the color without losing group opacity.
static void svg_apply_paint(const char* ts, int tlen, SvgPaint* pt) {
    int vlen = 0;
    const char* v = svg_attr(ts, tlen, "fill", &vlen);
    if (v) {
        int gidx = svg_grad_find(v, vlen);
        if (gidx >= 0) {
            pt->fill_grad = gidx;
            pt->has_fill = 1; pt->fill_none = 0;
        } else {
            uint8_t ar, ag, ab, aa;
            if (svg_color(v, vlen, &ar, &ag, &ab, &aa)) {
                pt->fill_grad = -1;
                pt->fr = ar; pt->fg = ag; pt->fb = ab;
                pt->has_fill = 1; pt->fill_none = 0;
                if (aa < 255) pt->fill_alpha = aa;
            } else {
                pt->fill_grad = -1;
                pt->fill_none = 1; pt->has_fill = 0;
            }
        }
    }
    v = svg_attr(ts, tlen, "stroke", &vlen);
    if (v) {
        int gidx = svg_grad_find(v, vlen);
        if (gidx >= 0) {
            pt->stroke_grad = gidx;
            pt->has_stroke = 1; pt->stroke_none = 0;
        } else {
            uint8_t ar, ag, ab, aa;
            if (svg_color(v, vlen, &ar, &ag, &ab, &aa)) {
                pt->stroke_grad = -1;
                pt->sr = ar; pt->sg = ag; pt->sb = ab;
                pt->has_stroke = 1; pt->stroke_none = 0;
                if (aa < 255) pt->stroke_alpha = aa;
            } else {
                pt->stroke_grad = -1;
                pt->stroke_none = 1; pt->has_stroke = 0;
            }
        }
    }
    int32_t num;
    if (svg_attr_num(ts, tlen, "stroke-width", &num) && num > 0)
        pt->stroke_w = num;
    if (svg_attr_num(ts, tlen, "fill-opacity", &num)) {
        if (num < 0) num = 0;
        if (num > FX_ONE) num = FX_ONE;
        pt->fill_alpha = (int32_t)(((int64_t)pt->fill_alpha * num) >> FX_SHIFT);
    }
    if (svg_attr_num(ts, tlen, "opacity", &num)) {
        if (num < 0) num = 0;
        if (num > FX_ONE) num = FX_ONE;
        pt->opacity = (int32_t)(((int64_t)pt->opacity * num) >> FX_SHIFT);
    }
    v = svg_attr(ts, tlen, "filter", &vlen);
    if (v) {
        int fidx = svg_filter_find(v, vlen);
        if (fidx >= 0) pt->filter = fidx;
    }
}

// Parse transform="translate(x[,y]) scale(sx[,sy]) ..." (the common
// icon cases; rotate/matrix/skew are ignored) and compose each op
// onto *m with a right-multiply — transform lists apply left to
// right, so M = M_old * Op.
static void svg_apply_transform(const char* ts, int tlen, SvgMtx* m) {
    int vlen = 0;
    const char* v = svg_attr(ts, tlen, "transform", &vlen);
    if (!v) return;
    const char* p = v;
    const char* end = v + vlen;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
        if (p >= end) break;
        const char* ns = p;
        while (p < end && *p != '(') p++;
        if (p >= end) break;
        int nlen = (int)(p - ns);
        p++;                          // past '('
        int32_t vals[2];
        int got = 0;
        while (p < end && *p != ')' && got < 2) {
            int32_t tmp;
            const char* r2 = svg_num(p, end, &tmp);
            if (r2 == p) { p++; continue; }
            p = r2;
            if (got < 2) vals[got++] = tmp;
        }
        while (p < end && *p != ')') p++;
        if (p < end) p++;             // past ')'
        if (nlen == 9 && ci_eq(ns, "translate", 9)) {
            int32_t tx = got > 0 ? vals[0] : 0;
            int32_t ty = got > 1 ? vals[1] : 0;
            m->e = m->e + fx_mul(tx, m->a) + fx_mul(ty, m->c);
            m->f = m->f + fx_mul(tx, m->b) + fx_mul(ty, m->d);
        } else if (nlen == 5 && ci_eq(ns, "scale", 5)) {
            int32_t sx = got > 0 ? vals[0] : FX_ONE;
            int32_t sy = got > 1 ? vals[1] : sx;
            // Post-multiply (M = M_old * S), matching the translate
            // branch — the scale acts on the POINT before the
            // accumulated transform, so the translation column (e/f)
            // is left untouched.  (Previously e/f were scaled too,
            // which pre-multiplied; combined translate+scale lists
            // like translate(A) scale(B) translate(C) then pushed
            // the result off-canvas.  Single-op icons were unaffected.)
            m->a = fx_mul(m->a, sx); m->b = fx_mul(m->b, sx);
            m->c = fx_mul(m->c, sy); m->d = fx_mul(m->d, sy);
        }
    }
}

// Map a user-space point through the element transform, then the
// viewBox → pixel mapping (fixed point, int64 intermediates).
static void rs_map(Raster* rs, const SvgMtx* m, int32_t vx, int32_t vy,
                   int32_t* px, int32_t* py) {
    int32_t ux = (int32_t)(((int64_t)m->a * vx + (int64_t)m->c * vy) >> FX_SHIFT) + m->e;
    int32_t uy = (int32_t)(((int64_t)m->b * vx + (int64_t)m->d * vy) >> FX_SHIFT) + m->f;
    *px = rs_x(rs, ux);
    *py = rs_y(rs, uy);
}

// Stroke half-width in pixels: stroke-width (user units) scaled by
// the average of the transform + viewBox scales.
static int32_t svg_sw(Raster* rs, const SvgMtx* m, int32_t stroke_w) {
    int32_t ax = m->a < 0 ? -m->a : m->a;
    int32_t dy = m->d < 0 ? -m->d : m->d;
    int32_t sx = fx_mul(ax, rs->sx);
    int32_t sy = fx_mul(dy, rs->sy);
    int32_t avg = (sx + sy) / 2;
    if (avg < FX_ONE / 16) avg = FX_ONE / 16;
    return fx_mul(stroke_w, avg) / 2;
}

// Precompute a gradient's pixel-space geometry for a shape bbox so
// grad_t_at() only needs plain pixel math afterwards.  Two units:
//   objectBoundingBox (default): coords are 0..1 fractions of the
//       bbox (x1..y2 / cx,cy,r are fixed 0..65536) → scale by bw/bh.
//   userSpaceOnUse: coords are user (viewBox) units → map through
//       rs_x/rs_y, and radii through rs->sx/rs->sy.
static void svg_grad_geom(Raster* rs, const SvgGrad* g, SvgBrush* br,
                          int32_t bx0, int32_t by0, int32_t bw, int32_t bh) {
    br->radial = g->radial;
    if (g->user_space) {
        if (!g->radial) {
            br->px1 = rs_x(rs, g->x1); br->py1 = rs_y(rs, g->y1);
            br->px2 = rs_x(rs, g->x2); br->py2 = rs_y(rs, g->y2);
        } else {
            br->pcx = rs_x(rs, g->cx); br->pcy = rs_y(rs, g->cy);
            br->prx = fx_mul(g->r, rs->sx);
            br->pry = fx_mul(g->r, rs->sy);
            br->pfx = rs_x(rs, g->fx); br->pfy = rs_y(rs, g->fy);
        }
    } else {
        // bx0/by0/bw/bh are integer pixels; x1 is fixed 0..65536, so
        // (x1 * bw) is already in fixed-point pixels.
        if (!g->radial) {
            br->px1 = (bx0 << FX_SHIFT) + g->x1 * bw;
            br->py1 = (by0 << FX_SHIFT) + g->y1 * bh;
            br->px2 = (bx0 << FX_SHIFT) + g->x2 * bw;
            br->py2 = (by0 << FX_SHIFT) + g->y2 * bh;
        } else {
            br->pcx = (bx0 << FX_SHIFT) + g->cx * bw;
            br->pcy = (by0 << FX_SHIFT) + g->cy * bh;
            br->prx = g->r * bw;
            br->pry = g->r * bh;
            br->pfx = (bx0 << FX_SHIFT) + g->fx * bw;
            br->pfy = (by0 << FX_SHIFT) + g->fy * bh;
        }
    }
    if (br->radial) {
        if (br->prx < FX_ONE) br->prx = FX_ONE;   // guard vs div-by-tiny
        if (br->pry < FX_ONE) br->pry = FX_ONE;
    }
}

// Build the fill/stroke brush for a shape (gradient fills need the
// shape bbox to map the gradient's 0..1 coordinates).
static void brush_fill(Raster* rs, SvgBrush* br, const SvgPaint* pt,
                       int32_t bx0, int32_t by0, int32_t bw, int32_t bh,
                       uint8_t alpha) {
    if (pt->fill_grad >= 0) {
        br->grad = &svg_grads[pt->fill_grad];
        svg_grad_geom(rs, br->grad, br, bx0, by0, bw, bh);
        br->a = alpha;
    } else {
        br->grad = NULL;
        br->r = pt->fr; br->g = pt->fg; br->b = pt->fb; br->a = alpha;
    }
}

static void brush_stroke(Raster* rs, SvgBrush* br, const SvgPaint* pt,
                         int32_t bx0, int32_t by0, int32_t bw, int32_t bh,
                         uint8_t alpha) {
    if (pt->stroke_grad >= 0) {
        br->grad = &svg_grads[pt->stroke_grad];
        svg_grad_geom(rs, br->grad, br, bx0, by0, bw, bh);
        br->a = alpha;
    } else {
        br->grad = NULL;
        br->r = pt->sr; br->g = pt->sg; br->b = pt->sb; br->a = alpha;
    }
}

// ─── Path flattening ─────────────────────────────────────────────
// Parses d= data, emitting line segments (fixed, viewBox coords)
// into px/py (capacity cap).  Returns point count.
static int svg_path_points(const char* d, int dlen,
                           int32_t* px, int32_t* py, int cap) {
    int n = 0;
    int32_t cx = 0, cy = 0;          // current point
    int32_t sx = 0, sy = 0;          // subpath start
    int32_t c1x = 0, c1y = 0;        // previous control point (S/T)
    int has_prev_ctrl = 0;
    const char* p = d;
    const char* end = d + dlen;

    // Cubic/quadratic Bernstein evaluation with safe fixed math:
    // weights are normalized to [0, 65536] before each multiply so
    // the products never overflow int64.
    //   cubic:   w0=u^3, w1=3u^2t, w2=3ut^2, w3=t^3 (u=1-t)
    //   result = (w0*P0 + w1*P1 + w2*P2 + w3*P3) >> 16
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
        if (p >= end) break;
        char c = *p;
        int rel = 0;
        if (c >= 'a' && c <= 'z') { rel = 1; c -= 32; }
        if (c < 'A' || c > 'Z') break;
        p++;

        if (c == 'Z') {
            if (n < cap) { px[n] = sx; py[n] = sy; n++; }
            cx = sx; cy = sy;
            has_prev_ctrl = 0;
            continue;
        }

        // Inner loop: consume coordinate pairs for this command
        // (e.g. "L 1 2 3 4" or "C a b c d e f g h i j k l" repeat).
        for (;;) {
            // Relative base — recomputed per pair so a relative
            // command chains from the point it just reached.
            int32_t rx = 0, ry = 0;
            if (rel) { rx = cx; ry = cy; }

            // Peek how many numbers remain for this command.
            int got = 0;
            int32_t vals[6];
            {
                const char* r = p;
                while (got < 6) {
                    int32_t tmp;
                    const char* r2 = svg_num(r, end, &tmp);
                    if (r2 == r) break;
                    vals[got++] = tmp;
                    r = r2;
                }
            }
            if (got == 0) break;

            if (c == 'M') {           // moveto (1 pair, or more = lineto)
                if (got < 2) break;
                cx = vals[0] + rx; cy = vals[1] + ry;
                sx = cx; sy = cy;
                if (n < cap) { px[n] = cx; py[n] = cy; n++; }
                has_prev_ctrl = 0;
                for (int i = 0; i < 2; i++) { int32_t tmp; p = svg_num(p, end, &tmp); }
                c = 'L';              // any extra pairs are lineto
                continue;
            } else if (c == 'L') {    // lineto
                if (got < 2) break;
                cx = vals[0] + rx; cy = vals[1] + ry;
                if (n < cap) { px[n] = cx; py[n] = cy; n++; }
                has_prev_ctrl = 0;
                for (int i = 0; i < 2; i++) { int32_t tmp; p = svg_num(p, end, &tmp); }
            } else if (c == 'H') {    // horizontal
                if (got < 1) break;
                cx = vals[0] + rx;
                if (n < cap) { px[n] = cx; py[n] = cy; n++; }
                has_prev_ctrl = 0;
                { int32_t tmp; p = svg_num(p, end, &tmp); }
            } else if (c == 'V') {    // vertical
                if (got < 1) break;
                cy = vals[0] + ry;
                if (n < cap) { px[n] = cx; py[n] = cy; n++; }
                has_prev_ctrl = 0;
                { int32_t tmp; p = svg_num(p, end, &tmp); }
            } else if (c == 'C') {    // cubic bezier
                if (got < 6) break;
                int32_t x0 = cx, y0 = cy;
                int32_t x1 = vals[0] + rx, y1 = vals[1] + ry;
                int32_t x2 = vals[2] + rx, y2 = vals[3] + ry;
                int32_t x3 = vals[4] + rx, y3 = vals[5] + ry;
                for (int k = 1; k <= 8; k++) {
                    int32_t t = fx_div(k * FX_ONE, 8 * FX_ONE);
                    int32_t u = FX_ONE - t;
                    int64_t u2 = (int64_t)u * u >> FX_SHIFT;
                    int64_t t2 = (int64_t)t * t >> FX_SHIFT;
                    int64_t w0 = u2 * u >> FX_SHIFT;
                    int64_t w3 = t2 * t >> FX_SHIFT;
                    int64_t w1 = 3 * (u2 * t >> FX_SHIFT);
                    int64_t w2 = 3 * (u * t2 >> FX_SHIFT);
                    int64_t bx = (w0*x0 + w1*x1 + w2*x2 + w3*x3) >> FX_SHIFT;
                    int64_t by = (w0*y0 + w1*y1 + w2*y2 + w3*y3) >> FX_SHIFT;
                    if (n < cap) { px[n] = (int32_t)bx; py[n] = (int32_t)by; n++; }
                }
                cx = x3; cy = y3;
                c1x = x2; c1y = y2; has_prev_ctrl = 1;
                for (int i = 0; i < 6; i++) { int32_t tmp; p = svg_num(p, end, &tmp); }
            } else if (c == 'S') {    // smooth cubic
                if (got < 4) break;
                int32_t x0 = cx, y0 = cy;
                int32_t x1 = has_prev_ctrl ? 2*cx - c1x : cx;
                int32_t y1 = has_prev_ctrl ? 2*cy - c1y : cy;
                int32_t x2 = vals[0] + rx, y2 = vals[1] + ry;
                int32_t x3 = vals[2] + rx, y3 = vals[3] + ry;
                for (int k = 1; k <= 8; k++) {
                    int32_t t = fx_div(k * FX_ONE, 8 * FX_ONE);
                    int32_t u = FX_ONE - t;
                    int64_t u2 = (int64_t)u * u >> FX_SHIFT;
                    int64_t t2 = (int64_t)t * t >> FX_SHIFT;
                    int64_t w0 = u2 * u >> FX_SHIFT;
                    int64_t w3 = t2 * t >> FX_SHIFT;
                    int64_t w1 = 3 * (u2 * t >> FX_SHIFT);
                    int64_t w2 = 3 * (u * t2 >> FX_SHIFT);
                    int64_t bx = (w0*x0 + w1*x1 + w2*x2 + w3*x3) >> FX_SHIFT;
                    int64_t by = (w0*y0 + w1*y1 + w2*y2 + w3*y3) >> FX_SHIFT;
                    if (n < cap) { px[n] = (int32_t)bx; py[n] = (int32_t)by; n++; }
                }
                cx = x3; cy = y3;
                c1x = x2; c1y = y2; has_prev_ctrl = 1;
                for (int i = 0; i < 4; i++) { int32_t tmp; p = svg_num(p, end, &tmp); }
            } else if (c == 'Q') {    // quadratic
                if (got < 4) break;
                int32_t x0 = cx, y0 = cy;
                int32_t x1 = vals[0] + rx, y1 = vals[1] + ry;
                int32_t x2 = vals[2] + rx, y2 = vals[3] + ry;
                for (int k = 1; k <= 8; k++) {
                    int32_t t = fx_div(k * FX_ONE, 8 * FX_ONE);
                    int32_t u = FX_ONE - t;
                    int64_t w0 = (int64_t)u * u >> FX_SHIFT;
                    int64_t w2 = (int64_t)t * t >> FX_SHIFT;
                    int64_t w1 = 2 * (u * t >> FX_SHIFT);
                    int64_t bx = (w0*x0 + w1*x1 + w2*x2) >> FX_SHIFT;
                    int64_t by = (w0*y0 + w1*y1 + w2*y2) >> FX_SHIFT;
                    if (n < cap) { px[n] = (int32_t)bx; py[n] = (int32_t)by; n++; }
                }
                cx = x2; cy = y2;
                c1x = x1; c1y = y1; has_prev_ctrl = 1;
                for (int i = 0; i < 4; i++) { int32_t tmp; p = svg_num(p, end, &tmp); }
            } else if (c == 'T') {    // smooth quadratic
                if (got < 2) break;
                int32_t x0 = cx, y0 = cy;
                int32_t x1 = has_prev_ctrl ? 2*cx - c1x : cx;
                int32_t y1 = has_prev_ctrl ? 2*cy - c1y : cy;
                int32_t x2 = vals[0] + rx, y2 = vals[1] + ry;
                for (int k = 1; k <= 8; k++) {
                    int32_t t = fx_div(k * FX_ONE, 8 * FX_ONE);
                    int32_t u = FX_ONE - t;
                    int64_t w0 = (int64_t)u * u >> FX_SHIFT;
                    int64_t w2 = (int64_t)t * t >> FX_SHIFT;
                    int64_t w1 = 2 * (u * t >> FX_SHIFT);
                    int64_t bx = (w0*x0 + w1*x1 + w2*x2) >> FX_SHIFT;
                    int64_t by = (w0*y0 + w1*y1 + w2*y2) >> FX_SHIFT;
                    if (n < cap) { px[n] = (int32_t)bx; py[n] = (int32_t)by; n++; }
                }
                cx = x2; cy = y2;
                c1x = x1; c1y = y1; has_prev_ctrl = 1;
                for (int i = 0; i < 2; i++) { int32_t tmp; p = svg_num(p, end, &tmp); }
            } else {
                break;
            }
            // More numbers for the same command?  Loop and re-peek.
            // Otherwise fall out to read the next command letter.
        }
    }
    return n;
}

// ─── Tag walking ─────────────────────────────────────────────────
// Drop shadows are rendered with a small 2-pass loop inside each
// shape branch (see svg_decode): pass 0 captures the element's
// silhouette into svg_shadow_buf, pass 1 composites the blurred
// shadow behind and then draws the element normally on top.  `sh`
// is true only when the element both carries an feDropShadow filter
// and has visible paint.

static const char* svg_next_tag(const char* p, const char* end,
                                const char** tstart, int* tlen,
                                int* closing, int* selfclose) {
    *tstart = NULL; *tlen = 0; *closing = 0; *selfclose = 0;
    while (p < end) {
        if (p + 3 < end && p[0]=='<' && p[1]=='!' && p[2]=='-' && p[3]=='-') {
            const char* q = p + 4;
            while (q + 2 < end && !(q[0]=='-' && q[1]=='-' && q[2]=='>')) q++;
            p = (q + 2 < end) ? q + 3 : end;
            continue;
        }
        if (p + 8 < end && p[0]=='<' && p[1]=='!' && p[2]=='[' && p[3]=='C' &&
            p[4]=='D' && p[5]=='A' && p[6]=='T' && p[7]=='A') {
            const char* q = p + 8;
            while (q + 2 < end && !(q[0]==']' && q[1]==']' && q[2]=='>')) q++;
            p = (q + 2 < end) ? q + 3 : end;
            continue;
        }
        if (p[0] != '<') { p++; continue; }
        const char* ts = p + 1;
        if (ts < end && *ts == '/') { *closing = 1; ts++; }
        const char* scan = ts;
        while (scan < end && *scan != '>') scan++;
        if (scan >= end) break;
        *selfclose = (scan > ts && scan[-1] == '/');
        *tstart = ts;
        *tlen = (int)(scan - ts);
        return scan + 1;
    }
    return end;
}

static void svg_name(const char* t, int tlen, char* out, int max) {
    int i = 0;
    while (i < tlen && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' &&
           t[i] != '/' && t[i] != '>' && i < max - 1) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        out[i] = c;
        i++;
    }
    out[i] = '\0';
}

// ─── Public API ──────────────────────────────────────────────────
// ─── Root <svg> info ─────────────────────────────────────────────
// Finds the <svg> element (looping past <?xml?>, <!DOCTYPE>, and
// comments — real-world files rarely start with the svg tag itself)
// and extracts width/height + viewBox.  Shared by svg_dims() and
// svg_decode() so both agree on the canvas and scaling.
typedef struct {
    int32_t vbx, vby, vbw, vbh;   // viewBox (fixed); 0 if absent
    int     has_viewbox;
    int32_t vw, vh;               // width/height attrs (fixed); 0 if absent
    int     has_w, has_h;
} SvgRoot;

static int svg_root_info(const uint8_t* data, int datalen, SvgRoot* rt) {
    if (!data || !rt || datalen < 8) return 0;
    rt->vbx = rt->vby = rt->vbw = rt->vbh = 0;
    rt->has_viewbox = 0;
    rt->vw = rt->vh = 0;
    rt->has_w = rt->has_h = 0;

    const char* p = (const char*)data;
    const char* end = p + datalen;
    while (p < end) {
        const char* ts; int tlen, closing, selfclose;
        p = svg_next_tag(p, end, &ts, &tlen, &closing, &selfclose);
        if (!ts) break;
        char name[16];
        svg_name(ts, tlen, name, sizeof(name));
        if (closing) break;   // no <svg> opener found
        if (name[0] != 's' || name[1] != 'v' || name[2] != 'g' || name[3] != '\0')
            continue;
        {
            // Ignore percentage/unit sizes ("100%") so the viewBox
            // (or the default canvas) drives the dimensions instead.
            int vlen = 0;
            const char* v = svg_attr(ts, tlen, "width", &vlen);
            int has_pct = 0;
            if (v) { for (int i = 0; i < vlen; i++) if (v[i] == '%') { has_pct = 1; break; } }
            rt->has_w = (v != NULL && !has_pct);
            if (rt->has_w) svg_num(v, v + vlen, &rt->vw);
            v = svg_attr(ts, tlen, "height", &vlen);
            has_pct = 0;
            if (v) { for (int i = 0; i < vlen; i++) if (v[i] == '%') { has_pct = 1; break; } }
            rt->has_h = (v != NULL && !has_pct);
            if (rt->has_h) svg_num(v, v + vlen, &rt->vh);
        }
        int vlen = 0;
        const char* vb = svg_attr(ts, tlen, "viewBox", &vlen);
        if (vb) {
            const char* q = vb;
            const char* qe = vb + vlen;
            int32_t vals[4];
            int got = 0;
            while (got < 4) {
                int32_t tmp;
                const char* r2 = svg_num(q, qe, &tmp);
                if (r2 == q) break;
                vals[got++] = tmp;
                q = r2;
            }
            if (got == 4) {
                rt->vbx = vals[0]; rt->vby = vals[1];
                rt->vbw = vals[2]; rt->vbh = vals[3];
                rt->has_viewbox = (rt->vbw > 0 && rt->vbh > 0);
            }
        }
        return 1;
    }
    return 0;
}

int svg_dims(const uint8_t* data, int datalen, int* w, int* h) {
    SvgRoot rt;
    if (!svg_root_info(data, datalen, &rt)) return 0;
    int out_w = 0, out_h = 0;
    if (rt.has_w && rt.vw > 0) out_w = fx_to_int(rt.vw);
    if (rt.has_h && rt.vh > 0) out_h = fx_to_int(rt.vh);
    if (out_w <= 0 && rt.vbw > 0) out_w = fx_to_int(rt.vbw);
    if (out_h <= 0 && rt.vbh > 0) out_h = fx_to_int(rt.vbh);
    if (out_w <= 0) out_w = 256;
    if (out_h <= 0) out_h = 256;
    if (out_w > 4096) out_w = 4096;
    if (out_h > 4096) out_h = 4096;
    *w = out_w; *h = out_h;
    return 1;
}

int svg_decode(const uint8_t* data, int datalen, SvgImage* img) {
    if (!data || !img || !img->pixels || datalen < 8) return 0;
    int W = 0, H = 0;
    if (!svg_dims(data, datalen, &W, &H)) return 0;
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) return 0;
    if ((int64_t)W * H * 4 > 1024 * 1024 * 4) return 0;   // sane cap

    img->width = (uint32_t)W;
    img->height = (uint32_t)H;
    for (int i = 0; i < W * H * 4; i++) img->pixels[i] = 0;

    Raster rs;
    rs.pix = img->pixels;
    rs.W = W; rs.H = H;
    rs.vbx = 0; rs.vby = 0; rs.vbw = W << FX_SHIFT; rs.vbh = H << FX_SHIFT;
    rs.sx = FX_ONE; rs.sy = FX_ONE;
    rs.shbuf = NULL;   // shadow capture off until rs_shadow_begin()

    // Scale setup from the root <svg> viewBox (shared parser that
    // also locates the tag past <?xml?>/<!DOCTYPE>/comments).
    {
        SvgRoot rt;
        if (svg_root_info(data, datalen, &rt) && rt.has_viewbox) {
            rs.vbx = rt.vbx; rs.vby = rt.vby;
            rs.vbw = rt.vbw; rs.vbh = rt.vbh;
            rs.sx = fx_div(W << FX_SHIFT, rt.vbw);
            rs.sy = fx_div(H << FX_SHIFT, rt.vbh);
        }
    }

    // ── Walk elements ────────────────────────────────────────────
    // Paint + transform state, inherited down through <g> groups via
    // a small frame stack (16 deep — deeper nests just clamp).
    static SvgPaint pt_stack[16];
    static SvgMtx   mtx_stack[16];
    int sp = 0;
    SvgPaint pt; svg_paint_default(&pt);
    SvgMtx   mtx; svg_mtx_identity(&mtx);
    int cur_grad = -1;                 // open <linearGradient> slot
    int cur_filter = -1;               // open <filter> slot
    svg_grad_n = 0;                    // fresh gradient set per decode
    svg_filter_n = 0;                  // fresh filter set per decode

    const char* p = (const char*)data;
    const char* end = p + datalen;
    while (p < end) {
        const char* ts; int tlen, closing, selfclose;
        p = svg_next_tag(p, end, &ts, &tlen, &closing, &selfclose);
        if (!ts) break;
        char name[16];
        svg_name(ts, tlen, name, sizeof(name));
        if (closing) {
            // Close an open gradient / restore the parent group
            // frame on </g>/</svg> (stray closing tags for leaf
            // shapes are ignored).
            if ((name[0]=='l' && name[1]=='i' && name[2]=='n' && name[3]=='e' &&
                 name[4]=='a' && name[5]=='r') ||
                (name[0]=='r' && name[1]=='a' && name[2]=='d' && name[3]=='i' &&
                 name[4]=='a' && name[5]=='l'))
                cur_grad = -1;
            if (name[0]=='f' && name[1]=='i' && name[2]=='l' && name[3]=='t' &&
                name[4]=='e' && name[5]=='r')
                cur_filter = -1;
            if (sp > 0 && (name[0] == 'g' ||
                           (name[0] == 's' && name[1] == 'v' && name[2] == 'g'))) {
                sp--;
                pt = pt_stack[sp];
                mtx = mtx_stack[sp];
            }
            continue;
        }

        // <linearGradient>/<radialGradient> defs — recorded for
        // url(#id) lookups.  gradientUnits may be objectBoundingBox
        // (default — coords are 0..1 fractions of the shape bbox) or
        // userSpaceOnUse (coords are viewBox units).
        int is_lin = (name[0]=='l' && name[1]=='i' && name[2]=='n' &&
                      name[3]=='e' && name[4]=='a' && name[5]=='r' &&
                      name[6]=='g' && name[7]=='r' && name[8]=='a' && name[9]=='d');
        int is_rad = (name[0]=='r' && name[1]=='a' && name[2]=='d' &&
                      name[3]=='i' && name[4]=='a' && name[5]=='l' &&
                      name[6]=='g' && name[7]=='r' && name[8]=='a' && name[9]=='d');
        if (is_lin || is_rad) {
            int vlen = 0;
            const char* idv = svg_attr(ts, tlen, "id", &vlen);
            cur_grad = (idv && vlen > 0) ? svg_grad_slot(idv, vlen) : -1;
            if (cur_grad >= 0) {
                SvgGrad* g = &svg_grads[cur_grad];
                g->nstops = 0;
                g->radial = is_rad;
                g->user_space = 0;
                const char* gu = svg_attr(ts, tlen, "gradientUnits", &vlen);
                if (gu && vlen == 14 && ci_eq(gu, "userSpaceOnUse", 14))
                    g->user_space = 1;
                if (is_rad) {
                    // defaults per spec: cx=cy=0.5, r=0.5, fx=cx, fy=cy
                    g->cx = FX_ONE / 2; g->cy = FX_ONE / 2; g->r = FX_ONE / 2;
                    svg_attr_pct(ts, tlen, "cx", &g->cx);
                    svg_attr_pct(ts, tlen, "cy", &g->cy);
                    svg_attr_pct(ts, tlen, "r", &g->r);
                    g->fx = g->cx; g->fy = g->cy;
                    svg_attr_pct(ts, tlen, "fx", &g->fx);
                    svg_attr_pct(ts, tlen, "fy", &g->fy);
                } else {
                    g->x1 = 0; g->y1 = 0; g->x2 = FX_ONE; g->y2 = 0;
                    svg_attr_pct(ts, tlen, "x1", &g->x1);
                    svg_attr_pct(ts, tlen, "y1", &g->y1);
                    svg_attr_pct(ts, tlen, "x2", &g->x2);
                    svg_attr_pct(ts, tlen, "y2", &g->y2);
                }
            }
            continue;
        }
        // <stop> inside the open gradient.
        if (cur_grad >= 0 && name[0]=='s' && name[1]=='t' && name[2]=='o' && name[3]=='p') {
            SvgGrad* g = &svg_grads[cur_grad];
            if (g->nstops < SVG_MAX_STOPS) {
                SvgStop* s = &g->stops[g->nstops];
                s->r = 0; s->g = 0; s->b = 0; s->a = 255;
                int32_t num;
                if (!svg_attr_pct(ts, tlen, "offset", &num)) num = 0;
                s->off = num;
                int vlen = 0;
                const char* c = svg_attr(ts, tlen, "stop-color", &vlen);
                if (c) svg_color(c, vlen, &s->r, &s->g, &s->b, &s->a);
                if (svg_attr_pct(ts, tlen, "stop-opacity", &num))
                    s->a = (uint8_t)fx_mul(num, 255);
                g->nstops++;
            }
            continue;
        }

        // <filter>/<feDropShadow> defs — feDropShadow is the one
        // primitive we rasterize (soft drop shadows).  Registered so
        // filter="url(#id)" on shapes can resolve it later.
        if (name[0]=='f' && name[1]=='i' && name[2]=='l' && name[3]=='t' &&
            name[4]=='e' && name[5]=='r') {
            int vlen = 0;
            const char* idv = svg_attr(ts, tlen, "id", &vlen);
            cur_filter = (idv && vlen > 0) ? svg_filter_slot(idv, vlen) : -1;
            continue;
        }
        if (cur_filter >= 0 && name[0]=='f' && name[1]=='e' && name[2]=='d' &&
            name[3]=='r') {             // feDropShadow primitive
            SvgFilter* f = &svg_filters[cur_filter];
            f->has_shadow = 1;
            int32_t num;
            if (svg_attr_num(ts, tlen, "dx", &num)) f->dx = num;
            if (svg_attr_num(ts, tlen, "dy", &num)) f->dy = num;
            if (svg_attr_num(ts, tlen, "stdDeviation", &num)) f->std_dev = num;
            int vlen = 0;
            const char* c = svg_attr(ts, tlen, "flood-color", &vlen);
            if (c) {
                uint8_t ar, ag, ab, aa;
                if (svg_color(c, vlen, &ar, &ag, &ab, &aa)) {
                    f->fr = ar; f->fg = ag; f->fb = ab;
                }
            }
            if (svg_attr_num(ts, tlen, "flood-opacity", &num)) {
                if (num < 0) num = 0;
                if (num > FX_ONE) num = FX_ONE;
                f->flood_a = fx_mul(num, 255);
            }
            continue;
        }

        // Groups / root <svg>: push a frame, then apply their attrs
        // so children inherit the updated paint + transform.  A
        // self-closed group has no children, so it's a no-op.
        if (name[0] == 'g' || (name[0] == 's' && name[1] == 'v' && name[2] == 'g')) {
            if (!selfclose) {
                if (sp < 16) { pt_stack[sp] = pt; mtx_stack[sp] = mtx; sp++; }
                svg_apply_paint(ts, tlen, &pt);
                svg_apply_transform(ts, tlen, &mtx);
            }
            continue;
        }

        // Leaf shapes: inherit group state, then apply own overrides
        // (a fresh local copy so the stack stays untouched).
        SvgPaint el = pt;
        SvgMtx   em = mtx;
        svg_apply_paint(ts, tlen, &el);
        svg_apply_transform(ts, tlen, &em);

        int has_fill = el.has_fill && !el.fill_none;
        int has_stroke = el.has_stroke && !el.stroke_none;

        // Drop-shadow filter active on this element (only meaningful
        // when there is paint to silhouette)?
        int sh = (el.filter >= 0 && el.filter < SVG_MAX_FILTERS &&
                  svg_filters[el.filter].has_shadow) &&
                 (has_fill || has_stroke);
        int fill_a = (int)el.fill_alpha * (int)el.opacity / 255;
        int stroke_a = (int)el.stroke_alpha * (int)el.opacity / 255;

        // Only shapes below; groups/unknown tags are skipped
        int is_shape = 0;
        if (name[0]=='r'&&name[1]=='e'&&name[2]=='c'&&name[3]=='t') is_shape = 1;
        else if (name[0]=='c'&&name[1]=='i'&&name[2]=='r'&&name[3]=='c') is_shape = 2;
        else if (name[0]=='e'&&name[1]=='l'&&name[2]=='l'&&name[3]=='i'&&name[4]=='p') is_shape = 3;
        else if (name[0]=='l'&&name[1]=='i'&&name[2]=='n'&&name[3]=='e') is_shape = 4;
        else if (name[0]=='p'&&name[1]=='o'&&name[2]=='l'&&name[3]=='y') is_shape = 5; // polyline/polygon
        else if (name[0]=='p'&&name[1]=='a'&&name[2]=='t'&&name[3]=='h') is_shape = 6;
        if (!is_shape) continue;

        // ── rect ─────────────────────────────────────────────
        if (is_shape == 1) {
            int32_t x = 0, y = 0, w = 0, h = 0;
            svg_attr_num(ts, tlen, "x", &x);
            svg_attr_num(ts, tlen, "y", &y);
            svg_attr_num(ts, tlen, "width", &w);
            svg_attr_num(ts, tlen, "height", &h);
            if (w > 0 && h > 0) {
                int32_t px0, py0, px1, py1, px2, py2, px3, py3;
                rs_map(&rs, &em, x, y, &px0, &py0);
                rs_map(&rs, &em, x + w, y, &px1, &py1);
                rs_map(&rs, &em, x + w, y + h, &px2, &py2);
                rs_map(&rs, &em, x, y + h, &px3, &py3);
                int32_t bx0 = px0, by0 = py0, bx1 = px0, by1 = py0;
                int32_t xp4[4] = {px0, px1, px2, px3};
                int32_t yp4[4] = {py0, py1, py2, py3};
                for (int k = 0; k < 4; k++) {
                    if (xp4[k] < bx0) bx0 = xp4[k];
                    if (xp4[k] > bx1) bx1 = xp4[k];
                    if (yp4[k] < by0) by0 = yp4[k];
                    if (yp4[k] > by1) by1 = yp4[k];
                }
                int ibx0 = fx_to_int(bx0), iby0 = fx_to_int(by0);
                int ibw = fx_to_int(bx1) - ibx0 + 1, ibh = fx_to_int(by1) - iby0 + 1;
                SvgBrush fbr, sbr;
                if (has_fill)
                    brush_fill(&rs, &fbr, &el, ibx0, iby0, ibw, ibh, (uint8_t)fill_a);
                if (has_stroke)
                    brush_stroke(&rs, &sbr, &el, ibx0, iby0, ibw, ibh, (uint8_t)stroke_a);
                // Rounded corners (rx/ry user units → pixels, like
                // ellipse radii); rx=0 degrades to a plain rect.
                int32_t rxu = 0, ryu = 0;
                svg_attr_num(ts, tlen, "rx", &rxu);
                svg_attr_num(ts, tlen, "ry", &ryu);
                if (ryu <= 0) ryu = rxu;
                if (rxu > 0 && ryu > 0) {
                    int32_t ax = em.a < 0 ? -em.a : em.a;
                    int32_t dy = em.d < 0 ? -em.d : em.d;
                    int32_t rxp = fx_mul(fx_mul(rxu, ax), rs.sx);
                    int32_t ryp = fx_mul(fx_mul(ryu, dy), rs.sy);
                    if (rxp > (bx1 - bx0) / 2) rxp = (bx1 - bx0) / 2;
                    if (ryp > (by1 - by0) / 2) ryp = (by1 - by0) / 2;
                    for (int shp = 0, shn = sh ? 2 : 1; shp < shn; shp++) {
                        if (shp == 0 && sh) rs_shadow_begin(&rs, &svg_filters[el.filter]);
                        if (shp == 1) rs_shadow_end(&rs);
                        if (has_fill)
                            rs_fill_rrect(&rs, bx0, by0, bx1, by1, rxp, ryp, &fbr);
                        if (has_stroke)
                            rs_stroke_rrect(&rs, bx0, by0, bx1, by1, rxp, ryp,
                                            svg_sw(&rs, &em, el.stroke_w), &sbr);
                    }
                } else {
                    for (int shp = 0, shn = sh ? 2 : 1; shp < shn; shp++) {
                        if (shp == 0 && sh) rs_shadow_begin(&rs, &svg_filters[el.filter]);
                        if (shp == 1) rs_shadow_end(&rs);
                        if (has_fill) rs_fill_poly(&rs, xp4, yp4, 4, &fbr);
                        if (has_stroke) {
                            int32_t hw = svg_sw(&rs, &em, el.stroke_w);
                            rs_stroke_line(&rs, px0, py0, px1, py1, hw, &sbr);
                            rs_stroke_line(&rs, px1, py1, px2, py2, hw, &sbr);
                            rs_stroke_line(&rs, px2, py2, px3, py3, hw, &sbr);
                            rs_stroke_line(&rs, px3, py3, px0, py0, hw, &sbr);
                        }
                    }
                }
            }
        }
        // ── circle / ellipse ──────────────────────────────────
        else if (is_shape == 2 || is_shape == 3) {
            int32_t cx = 0, cy = 0, rx = 0, ry = 0;
            if (is_shape == 2) {
                int32_t r = 0;
                svg_attr_num(ts, tlen, "cx", &cx);
                svg_attr_num(ts, tlen, "cy", &cy);
                svg_attr_num(ts, tlen, "r", &r);
                rx = r; ry = r;
            } else {
                svg_attr_num(ts, tlen, "cx", &cx);
                svg_attr_num(ts, tlen, "cy", &cy);
                svg_attr_num(ts, tlen, "rx", &rx);
                svg_attr_num(ts, tlen, "ry", &ry);
            }
            if (rx > 0 && ry > 0) {
                int32_t pcx, pcy;
                rs_map(&rs, &em, cx, cy, &pcx, &pcy);
                // Scale radii by the transform's x/y components then
                // the viewBox scale.  Exact for translate/scale — the
                // ellipse stays axis-aligned (no rotation support).
                int32_t ax = em.a < 0 ? -em.a : em.a;
                int32_t dy = em.d < 0 ? -em.d : em.d;
                int32_t prx = fx_mul(fx_mul(rx, ax), rs.sx);
                int32_t pry = fx_mul(fx_mul(ry, dy), rs.sy);
                int32_t ibx0 = fx_to_int(pcx - prx), iby0 = fx_to_int(pcy - pry);
                int ibw = fx_to_int(prx * 2) + 1, ibh = fx_to_int(pry * 2) + 1;
                SvgBrush fbr, sbr;
                if (has_fill)
                    brush_fill(&rs, &fbr, &el, ibx0, iby0, ibw, ibh, (uint8_t)fill_a);
                if (has_stroke)
                    brush_stroke(&rs, &sbr, &el, ibx0, iby0, ibw, ibh, (uint8_t)stroke_a);
                for (int shp = 0, shn = sh ? 2 : 1; shp < shn; shp++) {
                    if (shp == 0 && sh) rs_shadow_begin(&rs, &svg_filters[el.filter]);
                    if (shp == 1) rs_shadow_end(&rs);
                    if (has_fill)
                        rs_fill_ellipse(&rs, pcx, pcy, prx, pry, &fbr);
                    if (has_stroke) {
                        int32_t hw = svg_sw(&rs, &em, el.stroke_w);
                        static const int32_t cos32[32] = {
                            65536, 64277, 60547, 54491, 46341, 36410, 25080, 12785,
                            0, -12785, -25080, -36410, -46341, -54491, -60547, -64277,
                            -65536, -64277, -60547, -54491, -46341, -36410, -25080, -12785,
                            0, 12785, 25080, 36410, 46341, 54491, 60547, 64277 };
                        int32_t xp[32], yp[32];
                        for (int k = 0; k < 32; k++) {
                            int32_t ca = cos32[k], sa = cos32[(k + 8) % 32];
                            xp[k] = pcx + fx_mul(prx, ca);
                            yp[k] = pcy - fx_mul(pry, sa);
                        }
                        for (int k = 0; k < 32; k++) {
                            int k2 = (k + 1) % 32;
                            rs_stroke_line(&rs, xp[k], yp[k], xp[k2], yp[k2], hw, &sbr);
                        }
                    }
                }
            }
        }
        // ── line ──────────────────────────────────────────────
        else if (is_shape == 4) {
            if (has_stroke) {
                int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                svg_attr_num(ts, tlen, "x1", &x1);
                svg_attr_num(ts, tlen, "y1", &y1);
                svg_attr_num(ts, tlen, "x2", &x2);
                svg_attr_num(ts, tlen, "y2", &y2);
                int32_t px0, py0, px1, py1;
                rs_map(&rs, &em, x1, y1, &px0, &py0);
                rs_map(&rs, &em, x2, y2, &px1, &py1);
                int32_t bx0 = px0 < px1 ? px0 : px1, bx1 = px0 < px1 ? px1 : px0;
                int32_t by0 = py0 < py1 ? py0 : py1, by1 = py0 < py1 ? py1 : py0;
                SvgBrush sbr;
                brush_stroke(&rs, &sbr, &el, fx_to_int(bx0), fx_to_int(by0),
                             fx_to_int(bx1) - fx_to_int(bx0) + 1,
                             fx_to_int(by1) - fx_to_int(by0) + 1,
                             (uint8_t)stroke_a);
                for (int shp = 0, shn = sh ? 2 : 1; shp < shn; shp++) {
                    if (shp == 0 && sh) rs_shadow_begin(&rs, &svg_filters[el.filter]);
                    if (shp == 1) rs_shadow_end(&rs);
                    rs_stroke_line(&rs, px0, py0, px1, py1,
                                   svg_sw(&rs, &em, el.stroke_w), &sbr);
                }
            }
        }
        // ── polyline / polygon ────────────────────────────────
        else if (is_shape == 5) {
            int vlen = 0;
            const char* pts = svg_attr(ts, tlen, "points", &vlen);
            if (pts) {
                static int32_t xp[256], yp[256];
                int n = 0;
                const char* q = pts; const char* qe = pts + vlen;
                while (n < 256) {
                    int32_t a, b;
                    const char* r2 = svg_num(q, qe, &a);
                    if (r2 == q) break;
                    q = svg_num(r2, qe, &b);
                    if (q == r2) break;
                    rs_map(&rs, &em, a, b, &xp[n], &yp[n]);
                    n++;
                }
                if (n > 0) {
                    int32_t bx0 = xp[0], by0 = yp[0], bx1 = xp[0], by1 = yp[0];
                    for (int k = 1; k < n; k++) {
                        if (xp[k] < bx0) bx0 = xp[k];
                        if (xp[k] > bx1) bx1 = xp[k];
                        if (yp[k] < by0) by0 = yp[k];
                        if (yp[k] > by1) by1 = yp[k];
                    }
                    SvgBrush fbr, sbr;
                    if (has_fill)
                        brush_fill(&rs, &fbr, &el, fx_to_int(bx0), fx_to_int(by0),
                                   fx_to_int(bx1) - fx_to_int(bx0) + 1,
                                   fx_to_int(by1) - fx_to_int(by0) + 1,
                                   (uint8_t)fill_a);
                    if (has_stroke)
                        brush_stroke(&rs, &sbr, &el, fx_to_int(bx0), fx_to_int(by0),
                                     fx_to_int(bx1) - fx_to_int(bx0) + 1,
                                     fx_to_int(by1) - fx_to_int(by0) + 1,
                                     (uint8_t)stroke_a);
                    int is_polygon = (name[4]=='g' && name[5]=='o' && name[6]=='n');
                    for (int shp = 0, shn = sh ? 2 : 1; shp < shn; shp++) {
                        if (shp == 0 && sh) rs_shadow_begin(&rs, &svg_filters[el.filter]);
                        if (shp == 1) rs_shadow_end(&rs);
                        if (is_polygon && has_fill && n >= 3)
                            rs_fill_poly(&rs, xp, yp, n, &fbr);
                        if (has_stroke) {
                            int32_t hw = svg_sw(&rs, &em, el.stroke_w);
                            int segs = is_polygon ? n : n - 1;
                            for (int k = 0; k < segs; k++) {
                                int k2 = (k + 1) % n;
                                rs_stroke_line(&rs, xp[k], yp[k], xp[k2], yp[k2], hw, &sbr);
                            }
                        }
                    }
                }
            }
        }
        // ── path ──────────────────────────────────────────────
        else if (is_shape == 6) {
            int vlen = 0;
            const char* d = svg_attr(ts, tlen, "d", &vlen);
            if (d) {
                static int32_t xp[1024], yp[1024];
                int n = svg_path_points(d, vlen, xp, yp, 1024);
                if (n > 0) {
                    // Map the flattened polyline through the element's
                    // transform then the viewBox (in place — paths
                    // historically bypassed rs_x/rs_y, a latent bug
                    // that mis-placed icons with viewBoxes).
                    for (int k = 0; k < n; k++)
                        rs_map(&rs, &em, xp[k], yp[k], &xp[k], &yp[k]);
                    int32_t bx0 = xp[0], by0 = yp[0], bx1 = xp[0], by1 = yp[0];
                    for (int k = 1; k < n; k++) {
                        if (xp[k] < bx0) bx0 = xp[k];
                        if (xp[k] > bx1) bx1 = xp[k];
                        if (yp[k] < by0) by0 = yp[k];
                        if (yp[k] > by1) by1 = yp[k];
                    }
                    SvgBrush fbr, sbr;
                    if (has_fill)
                        brush_fill(&rs, &fbr, &el, fx_to_int(bx0), fx_to_int(by0),
                                   fx_to_int(bx1) - fx_to_int(bx0) + 1,
                                   fx_to_int(by1) - fx_to_int(by0) + 1,
                                   (uint8_t)fill_a);
                    if (has_stroke)
                        brush_stroke(&rs, &sbr, &el, fx_to_int(bx0), fx_to_int(by0),
                                     fx_to_int(bx1) - fx_to_int(bx0) + 1,
                                     fx_to_int(by1) - fx_to_int(by0) + 1,
                                     (uint8_t)stroke_a);
                    for (int shp = 0, shn = sh ? 2 : 1; shp < shn; shp++) {
                        if (shp == 0 && sh) rs_shadow_begin(&rs, &svg_filters[el.filter]);
                        if (shp == 1) rs_shadow_end(&rs);
                        if (has_fill && n >= 3)
                            rs_fill_poly(&rs, xp, yp, n, &fbr);
                        if (has_stroke) {
                            int32_t hw = svg_sw(&rs, &em, el.stroke_w);
                            for (int k = 0; k < n - 1; k++)
                                rs_stroke_line(&rs, xp[k], yp[k], xp[k+1], yp[k+1], hw, &sbr);
                        }
                    }
                }
            }
        }
    }
    return 1;
}
