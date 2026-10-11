// flashpix.cpp - see flashpix.h.
#include "flashpix.h"

#include <algorithm>

namespace rb {
namespace pix {

Window MakeWindow(int W, int H, bool showAll) {
    Window w;
    if (!showAll) {  // noScale, stage.align "" (centred)
        w.kx = w.ky = F32(1.0 / 20);
        w.tx = (W - 550) / 2;
        w.ty = (H - 400) / 2;
        return w;
    }
    const float kx = F32(W / 11000.0), ky = F32(H / 8000.0);
    if (W == (int)std::floor(1.375 * H + 0.625) && 8 * W != 11 * H) {
        w.kx = kx, w.ky = ky, w.hidden = true;
        return w;
    }
    const float k = std::min(kx, ky);
    w.kx = w.ky = k;
    w.tx = (int64_t)std::floor((W - std::floor(11000.0 * k + 0.5)) / 2);
    w.ty = (int64_t)std::floor((H - std::floor(8000.0 * k + 0.5)) / 2);
    return w;
}

DevMatrix RootMatrix(const Window& w) { return DevMatrix{F32(4.0 * w.kx), 0, 0, F32(4.0 * w.ky), 4 * w.tx, 4 * w.ty}; }

// Float concat 0x004c37e0: entries are float sums of products, the child's translation goes through the parent
// matrix and is rounded to a whole subpixel (a quarter window pixel) at every nesting level.
DevMatrix Concat(const DevMatrix& p, double la, double lb, double lc, double ld, int64_t ltx, int64_t lty) {
    const float a = F32(la), b = F32(lb), c = F32(lc), d = F32(ld);
    DevMatrix r;
    r.a = F32((double)b * p.c + (double)a * p.a);
    r.b = F32((double)b * p.d + (double)a * p.b);
    r.c = F32((double)c * p.a + (double)d * p.c);
    r.d = F32((double)c * p.b + (double)d * p.d);
    const double fx = F32((double)ltx), fy = F32((double)lty);
    r.tx = p.tx + Rint(F32(fx * p.a + fy * p.c));
    r.ty = p.ty + Rint(F32(fx * p.b + fy * p.d));
    return r;
}

void Probe(const Window& w, int64_t Tx, int64_t Ty, int64_t& px, int64_t& py) {
    px = 4 * (Rint(F32((double)Tx * w.kx)) + w.tx);
    py = 4 * (Rint(F32((double)Ty * w.ky)) + w.ty);
}

namespace {
struct Edge {
    int64_t x0, y0, cx, cy, x1, y1;
    bool flat;
};
inline int64_t PieceSize(int64_t w, int64_t h) { return (int64_t)std::nearbyint(std::sqrt((double)F32((double)(w * w + h * h)))); }
// 0x005c5bee: does the edge cross the ray from P towards +x? A curve piece whose box contains P is halved (integer
// midpoints, >> 1) until it is at most 4 subpixels across; such a piece counts as crossing if P lies in its height
// range, whatever P's x inside the box. This makes slanted edges fuzzy by up to one window pixel.
int Cross(const Edge& e, int64_t px, int64_t py, int depth) {
    int64_t xmin = std::min(e.x0, e.x1), xmax = std::max(e.x0, e.x1), ymin = std::min(e.y0, e.y1), ymax = std::max(e.y0, e.y1);
    if (!e.flat) xmin = std::min(xmin, e.cx), xmax = std::max(xmax, e.cx), ymin = std::min(ymin, e.cy), ymax = std::max(ymax, e.cy);
    if (!(px <= xmax && ymin <= py && py < ymax)) return 0;
    if (xmin <= px && py <= ymax && depth < 12 && PieceSize(xmax - xmin, ymax - ymin) > 4) {
        const int64_t ax = (e.x0 + e.cx) >> 1, ay = (e.y0 + e.cy) >> 1, bx = (e.cx + e.x1) >> 1, by = (e.cy + e.y1) >> 1;
        const int64_t mx = (ax + bx) >> 1, my = (ay + by) >> 1;
        return Cross(Edge{e.x0, e.y0, ax, ay, mx, my, e.flat}, px, py, depth + 1) +
               Cross(Edge{mx, my, bx, by, e.x1, e.y1, e.flat}, px, py, depth + 1);
    }
    return (std::min(e.y0, e.y1) <= py && py < std::max(e.y0, e.y1)) ? 1 : 0;
}
const int64_t kTri[3][2] = {{0, 0}, {60, -193}, {120, 0}};
}  // namespace

bool SpikeShapeHit(const DevMatrix& now, const DevMatrix& drawn, int64_t px, int64_t py) {
    // bounding box of the shape with the current matrix (corners through the vertex mapping), inclusive
    int64_t bx0 = INT64_MAX, bx1 = INT64_MIN, by0 = INT64_MAX, by1 = INT64_MIN;
    for (int64_t x : {0, 120})
        for (int64_t y : {-193, 0}) {
            int64_t ox, oy;
            Xform(now, x, y, ox, oy);
            bx0 = std::min(bx0, ox), bx1 = std::max(bx1, ox), by0 = std::min(by0, oy), by1 = std::max(by1, oy);
        }
    // 0x004b4bd0 (shape device bounds, SWF > 8): xmax and ymax grow by (aa + 1) / 2 = 2 subpixels at quality HIGH
    // (aa = 4). Invisible in a still scene, but it decides hits on the stale edges of a moving one.
    bx1 += kBoundsPad, by1 += kBoundsPad;
    if (px < bx0 || px > bx1 || py < by0 || py > by1) return false;
    // edges as last drawn; straight edges are curves with the control point at the floored midpoint, flat
    int64_t vx[3], vy[3];
    for (int i = 0; i < 3; ++i) Xform(drawn, kTri[i][0], kTri[i][1], vx[i], vy[i]);
    int odd = 0;
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        const Edge e{vx[i], vy[i], (vx[i] + vx[j]) >> 1, (vy[i] + vy[j]) >> 1, vx[j], vy[j], true};
        if (std::min(e.y0, e.y1) <= py && py < std::max(e.y0, e.y1) && Cross(e, px, py, 0)) odd ^= 1;  // even-odd fill
    }
    return odd != 0;
}

}  // namespace pix
}  // namespace rb
