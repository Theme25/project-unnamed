# Flash Player 11.4 internals relevant to rbsim (decompiled)

Source: the team's projector `Flash_Player_11.4_-_32_Bit_-_MaxPatch.exe`, which is the unmodified
Adobe standalone player `SAFlashPlayer.exe` 11.4.402.287. It is 32-bit, not packed, and has no symbols.
Decompiled with Ghidra 11.1.2 (headless) and capstone. All addresses are virtual addresses at the default
image base 0x400000.

Everything the simulator does at the display layer is now the player's own arithmetic, ported from
the code below. Before this, it was a model fitted to calibration dumps.

## 1. How the native code was found

- The player embeds its AS3 class library (`playerglobal`) as an uncompressed ABC block at 0x00a9e600.
  The avmplus core ABC (`Math`, ...) sits at 0x00b409a8.
- Each block has a native method table of 12-byte entries `{method id, thunk, handler}`:
  - playerglobal: 0x00a97378, 1,812 entries;
  - core: around 0x00b3f9c8.
- Mapping method ids to `Class.member` from the ABC traits names every native entry point. Table ids
  equal the ABC method index − 1 in these builds; check by semantics (for example, `Math.sqrt` →
  `fsqrt`).

| AS3 | entry |
|---|---|
| `DisplayObject.x` / `y` setter | 0x004ebe3d → 0x00551720 → 2D setter 0x004279c0 (cases 0/1) |
| `DisplayObject.rotation` / `scaleX/Y` setter | 0x004279c0 case 10 → 0x004b2600 (AS3 movies) |
| `DisplayObject.rotation` getter | 0x005506a3 → 0x0040d0f0 case 10 |
| `localToGlobal` / `globalToLocal` | 0x00551fd2 / 0x00551fc4 → 0x00550fc3 → 0x0040c970 / 0x0040cbc0 |
| `getBounds` | 0x00551fe0 → 0x005512ee → 0x005510ac |
| `hitTestObject` / `hitTestPoint` | `_hitTest` 0x005501fa |
| `Math.sin` / `cos` | MSVC `_CIsin` 0x009fdec0 / `_CIcos` 0x009fe3e0 |

## 2. Numbers and twips

- **`Math.sin`/`cos`:** MSVC's `_CIsin` checks `__use_sse2_mathfcns` and the default MXCSR/x87 masks,
  then jumps to `__sin_pentium4` (0x00a090a0). That is the Intel SSE2 LIBM routine in
  `src/libm_intel.S`. Without SSE2 it would fall back to x87 `fsin`.
- **`x`/`y` setters:** NaN → 0, then `fmul 20.0` and `_ftol2_sse` (`cvttsd2si`). The result is whole
  twips, truncated toward zero.
- **`localToGlobal`/`globalToLocal` input and output:**
  - input: NaN → 0, then `× 20` and truncation;
  - the point is transformed in integer twips;
  - output: the result `/ 20.0` as a double.
- **x87 precision:** the player relies on the Windows default (53-bit precision). The float32 code
  below stores to `float` explicitly where it rounds.

## 3. Rotation → matrix (fixed point)

- **Rotation setter** (0x004b2600):
  - the written value is kept as a double (+0x58);
  - `rotFx = trunc(rotation × 65536)`;
  - `sx = trunc(scaleX/100 × 65536)`;
  - `a = rnd(sx·cos(rotFx))`, `b = rnd(sx·sin(rotFx))`, `c = −rnd(sy·sin(rotFx+skew))`,
    `d = rnd(sy·cos(rotFx+skew))`, with `rnd(p) = (p >> 16) + bit15` (round half up).
- **Sine** (0x00542d70) and **cosine** (0x00542df0):
  - reduce into [0, 360) with unsigned division, then fold by symmetry;
  - core 0x00542cb0 reads a 0.25° table at 0x00ae1e98 that equals `trunc(sin(i·0.25°)·2³¹)`;
  - interpolation: `step × (low 14 bits) >> 16`, rounded half up. This is a quarter of the proper
    weight, a player bug that the old fitted model had also found;
  - the result is rounded half up from 2³¹ to 2¹⁶.
- **Comparison with the old model:** the fitted model (`trunc(sin·2³⁰)`) was wrong on 1,622 of the
  26,214,401 fixed rotations in [−200°, 200°], by 1/65536 in one entry. None of them occurred in any
  log or dump.
- **Fixed-point atan2** (0x00542fb0):
  - `fixdiv` (0x005c5e57) computes the ratio, rounded half up;
  - |t| > 1 folds to `90 − atan(1/t)`;
  - the core (0x00542e90) is a 10-node Newton-form polynomial in 2²⁵ fixed point, with nodes
    0.9 … 0.0001, 0;
  - quadrant fix-up last.
- **Timeline placement** (0x004ae590) stores `rotation = atan2fx(b, a) / 65536`. This reproduces all
  46 measured `kTimelineRotations` (for example Level 8 `killSpusk2`: −179.50430297851562).
  `Transform.matrix =` (0x004ae8c0) uses x87 `fpatan` instead, which is the E9b behaviour.

## 4. Matrices are float32 for SWF ≥ 8

- `0x0040a8f0` tests a byte of the movie (+0x39 > 7, the SWF version; Red Ball is v10). When it is
  set, identity, concat and every derived matrix are **float32**: entries are floats, translations
  are whole twips.
- The 16.16 values from the timeline or the rotation setter convert exactly
  (`int × (float)2⁻¹⁶`, 0x004c24c0).

| routine | float32 branch |
|---|---|
| point transform 0x004c30d0 | `X = tx + rint(f32(a·f32(x) + c·f32(y)))`, `Y = ty + rint(f32(d·f32(y) + b·f32(x)))`, where `rint` = `fistp`, half to even |
| inverse 0x004c3ba0 | `det = f32(d·a − c·b)`; `a' = f32(d/det)`, `b' = f32(−b/det)`, `c' = f32(−c/det)`, `d' = f32(a/det)`; `tx' = −rint(f32(ty·c' + a'·f32(tx)))`, `ty' = −rint(f32(ty·d' + b'·f32(tx)))` (note `ty` exact, `tx` rounded to float first). If b = c = 0: `1/a`, `1/d`, `−rint(f32(f32(t)·entry))` |
| concat 0x004c37e0 | entries as float sums of products; translation `t2 + rint(f32(…))` |
| rect transform 0x004c33b0 | b = c = 0: each side `t + rint(f32(v·a))`; otherwise the 4 corners through the point transform, then min/max |

- With this arithmetic the E11 turned-spike sweeps match **390,670 / 390,670**.
- The 5 rows the old double-precision fit missed were float32 rounding of the inverse's sums. The sum
  is rounded to float32 before `fistp`, and float32 spacing there is ~0.004 twip.

## 5. Bounds and `hitTestObject`

- Rectangles are `{xmin, xmax, ymin, ymax}` in twips; `0x7ffffff` marks an empty rectangle.
- **`hitTestObject`** (0x005501fa, branch `!useXY`):
  - both objects' global bounds come from 0x0040c5f0 (concat to the root, then 0x004b1700, which
    unions each child's local rect transformed by the concatenated matrix);
  - they are compared **inclusively** (0x004491e0: touching counts).
- **`getBounds(target)`** (0x005510ac, SWF > 9):
  - the object's local bounds;
  - its 4 corners → `localToGlobal(obj)` → `globalToLocal(target)`;
  - min/max.
- **`levelAim`:** Level 11's turned flag is the union of each child's transformed corners (E12
  148,074 / 148,074).

## 6. `hitTestPoint(x, y, shapeFlag = true)` — the real game's spikes (MATHSPIKES 0)

Branch `useXY && shapeFlag` of 0x005501fa:

1. The point (× 20, truncated) is taken from stage to **window** coordinates (0x0040c970 with the
   window-matrix flag), then multiplied by the player's anti-aliasing factor (0x00555950, field +0x48).
2. 0x004af380 / 0x004af060 build the matrix to the same device space. It includes the window/stage
   matrix and a `scale(+0x48 / 20)` (twips → device units).
3. 0x004b8450 walks the display tree. For each shape whose device-space bounds contain the point,
   it builds the shape's **render edge list** in that matrix (vtable +0x14) and runs 0x00557440.
4. 0x00557440 casts a ray toward +x:
   - an edge counts if `ymin ≤ py < ymax`;
   - parity is toggled per fill style (winding for type 2);
   - the point hits if any fill ends up odd.
5. The per-edge test 0x005c5bee treats edges as quadratic curves `{x0,y0, cx,cy, x1,y1}`:
   - it subdivides at integer midpoints (`>> 1`) only while the point lies inside the piece's bounding
     box, the piece's size (0x00594303) exceeds **4 device units**, and depth < 12;
   - the remaining piece counts as a crossing whenever `py` is within its chord's y-range and the
     point is not right of the piece's box. **x is ignored inside that box.**

Consequences for the real game, inferred from the code:
- Shape hit areas are exact only to about 4 device units. With 4× anti-aliasing (quality HIGH,
  device unit = ¼ window pixel), that is **one window pixel**.
- Inside that last piece a point counts as left of the edge, so hit areas lean toward +x.
- Everything is in window coordinates, so the result depends on:
  - the window scale (showAll, `min(W/550, H/400)` as float32);
  - the letterbox offset;
  - the quality (anti-aliasing factor);
  - the camera position.

This is the mechanism behind the side quest's measurements (¼-pixel render grid). rbsim now models the whole
test bit-exactly (`--mathspikes 0`, `src/flashpix.*`, `docs/RESOLUTION.md`). The remaining pieces, also from the
code:

- **Device bounds** (vtable +0x10 → 0x004b4bd0). The shape bounds go through the rect transform 0x004c33b0
  (b = c = 0: `tx + rint(f32(x·a))` per side), widened by the stroke padding (none for the spikes). For
  SWF > 8, `xmax` and `ymax` then grow by `(aa + 1) / 2`, which is 2 subpixels at quality HIGH. 0x004b8450 swaps
  these bounds in, computed with the current matrix, for the test. It then restores the bounds of the last
  redraw.
- **Edges** (vtable +0x14; the shape-record reader 0x004acbb0). Straight edges (record type 2) become curves whose control point is the
  `>> 1` midpoint, flagged as lines. Points go through the float32 point transform.
- **Edge cache.** The list is built at redraw time. During a hit test it is not rebuilt for a pure translation, so
  after a camera step the test uses the previous frame's outline inside the current box. This was measured with
  per-frame `hitTestPoint` scans and fixes the death frame of real-time playbacks at every window size tried.
