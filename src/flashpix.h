// flashpix.h - Flash Player 11.4's hitTestPoint(x, y, shapeFlag = true) for Red Ball's spike shape, at any window
// size. This is the spike test of the real game (Tournament Edition) and of the Practice Hack with MATHSPIKES 0.
//
// Ported from the decompiled player (docs/FLASH_PLAYER_RE.md section 6) and the WHG resolution findings, and
// checked against hitTestPoint sweeps recorded with the real 11.4 projector at many window sizes
// (docs/RESOLUTION.md). Units: stage twips in, window subpixels (1/4 window pixel) inside.
#pragma once
#include <cmath>
#include <cstdint>

namespace rb {
namespace pix {

inline float F32(double v) { return (float)v; }
inline int64_t Rint(float v) { return (int64_t)std::nearbyint((double)v); }  // x87 fistp: half to even

// Stage (550 x 400) -> window. showAll: scale = float32(W/11000) or float32(H/8000) (window px per twip), the
// smaller one, letterbox bars in whole pixels; a "hidden" size (W == floor(1.375 H + 0.625), not exactly 11:8)
// keeps both scales and has no bars. noScale: 1:1, the stage centred in the window.
struct Window {
    float kx = 1.0f / 20, ky = 1.0f / 20;  // window pixels per twip
    int64_t tx = 0, ty = 0;                // letterbox offset (window pixels)
    bool hidden = false;
};
Window MakeWindow(int width, int height, bool showAll);

// A display matrix into window subpixels: entries are float32 (subpixels per twip), the translation whole subpixels.
struct DevMatrix {
    float a, b, c, d;
    int64_t tx, ty;
};
DevMatrix RootMatrix(const Window& w);                               // stage twips -> window subpixels
DevMatrix Concat(const DevMatrix& parent, double a, double b, double c, double d, int64_t tx, int64_t ty);
inline void Xform(const DevMatrix& m, int64_t x, int64_t y, int64_t& ox, int64_t& oy) {
    ox = m.tx + Rint(F32((double)m.a * F32((double)x) + (double)m.c * F32((double)y)));
    oy = m.ty + Rint(F32((double)m.d * F32((double)y) + (double)m.b * F32((double)x)));
}

// The window subpixel a stage point is tested at: hitTestPoint takes the stage point in pixels, cuts it to whole
// twips, maps it to a whole window pixel and multiplies by 4. Tx/Ty = stage twips as Flash computes them.
void Probe(const Window& w, int64_t Tx, int64_t Ty, int64_t& px, int64_t& py);
// localToGlobal hands back twips / 20.0 as a double, hitTestPoint multiplies by 20 and truncates.
inline int64_t StageRoundTrip(int64_t twips) { return (int64_t)std::trunc((double)twips / 20.0 * 20.0); }

// Shape 56 (the Shipik triangle): (0,0) -> (60,-193) -> (120,0), one fill, bounds 0..120 x -193..0 twips.
// boxNow: the shape matrix now (bounding-box test); drawn: the matrix of the last redraw (edges). Flash keeps the
// edge list of the last redraw, so a shape moved since then is its old outline cut to its new box. The box is
// inclusive and its right and bottom sides are kBoundsPad subpixels further out (see flashpix.cpp).
constexpr int64_t kBoundsPad = 2;
bool SpikeShapeHit(const DevMatrix& boxNow, const DevMatrix& drawn, int64_t px, int64_t py);

}  // namespace pix
}  // namespace rb
