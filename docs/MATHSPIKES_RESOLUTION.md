# MATHSPIKES and the Flash window resolution

**Short version.** MATHSPIKES 1 (the Practice Hack's "math" spikes) is not pure math. Every control point
passes through Flash's display transforms (`localToGlobal` / `globalToLocal`), and those round to whole
twips. For **plain spikes** (unturned, or turned by exactly 90/180/270°) the rounding is lossless, so the
result cannot depend on the window. For **rotated or scaled spikes** (Levels 9, 10, 11, 15, 17) the
inverse transform in `Shipik.testPoint` rounds twice, to whole twips, in **absolute coordinates**:
camera position plus the window/letterbox transform. Those roundings land on a different grid for each
window size, so a control point that grazes a rotated spike edge can be inside at one resolution and
outside at another. **MATHSPIKES 2** (below) does the same test in Level coordinates with plain Numbers,
so it cannot depend on the window. It is bit-identical to MATHSPIKES 1 on every plain spike.

## 1. Where the resolution gets in

`Shipik.testPoint(globalPoint)` (decompiled):

```as3
point = this.p.globalToLocal(point);                                   // (1) stage -> Level
if (!this.getBounds(this.p).containsPoint(point)) return false;        // (2)
var pt:Point = globalToLocal(this.p.localToGlobal(point.subtract(cover))); // (3) Level -> stage -> Shipik
return PointInTriangle(pt);
```

`PlayerBox.HitTestObjectControlPoints` gets `globalPoint` from `pb.localToGlobal(testPoints[i])` (step 0).

Flash runs all of these through the object's *concatenated* matrix. Adobe's documentation of
`Transform.concatenatedMatrix` says that for resizable content this matrix also accounts for the
difference between stage and window coordinates. The verified model (`docs/STATS_LOGGING.md` §3.5–3.11):

- **Forward transforms** (steps 0, 3a): local point truncated to twips, 16.16 matrix applied, result
  rounded to a whole twip.
- **Inverse transforms** (steps 1, 3b): the inverse matrix is taken in doubles. Its **translation is
  rounded to a whole twip**, and so is the **result** (E11 fit, 390,665 / 390,670 rows).

**Plain spikes.** Every matrix entry is 0 or ±1, so whole twips map to whole twips. Neither rounding
loses anything, and an extra whole-twip offset cancels exactly.

**Rotated/scaled spikes.** The inverse translation `-(ia·TX + ic·TY)` and the result
`ia·GX + ic·GY + itx` are not whole numbers. Both get rounded, and both depend on the absolute
`TX`/`GX` (spike origin and point in the coordinates Flash rounds in). The E11 fit already showed this:
leaving the camera out of `TX`/`GX` takes the sweep from 5 to 1,315 disagreements. A different window
moves `TX`/`GX` again, so the triangle's edges shift by up to about one twip (more on the scaled
Level 15 spikes, in local units), in a pattern that changes with the resolution.

Spike "death" is decided at a threshold, so a grazing contact flips when its margin is below that jitter.
Most routes never graze a rotated spike within a twip. A route that slides along one for many frames
eventually does.

## 2. How big the effect is (simulator, `rbsim`)

The window transform itself has not been measured yet; see §4. Under the simplest hypothesis (**A**: an
extra whole-twip translation, the letterbox offset, enters `TX`/`GX`):

- Random ball positions grazing a rotated spike: about **1.5–2 %** change hit/miss somewhere over a range
  of offsets (a one-off experiment). For plain spikes the figure is **0** (self-test 20).
- The 15 E11 boundary rows in the self-tests: 15/15 in the reference window, as few as **11/15** with an
  offset of up to 10 px (self-test "E11 boundary rows").

Hypothesis **B** (`--stage-scale`, `BallHitsSpikeWindow`) runs every conversion through a showAll scale.
It predicts that **plain spikes change too** (about 0.1–0.4 % of grazing states, from re-rounding the
ball's control points in finer window twips). Your report says only rotated spikes are inconsistent,
which favours A or something close to it. The data in §4 decides.

## 3. MATHSPIKES 2

Files: `docs/mathspikes2/Shipik.as`, `docs/mathspikes2/PlayerBox.as` (the original classes plus the new
branch). In a ready-made SWF only these two classes are recompiled; every other method's bytecode is
unchanged apart from method numbering. Enable it with `/config mathspikes 2`.

What it computes:

1. **Control points** are produced the same way as `localToGlobal`, but in **Level** coordinates:
   - truncate `testPoints[i]` to twips;
   - apply the ball's 16.16 matrix from `transform.matrix`;
   - round to a twip;
   - add the ball's `x`/`y` in twips.

   The window, zoom and camera never enter.
2. **Spike matrix:** the Level→Shipik matrix is concatenated in Numbers from each clip's own
   `transform.matrix` (Shipik, then its Ships10), with translations in whole twips. It never uses
   `concatenatedMatrix`, because that one includes the window. The inverse is computed in Numbers, once
   per spike (spikes never move).
3. **Bounds:** the exact Level-space box of the shape's `(0,−9.65)–(6,0)`. It is tested on the
   **unshifted** point, as before, so the spike glitch is unchanged.
4. **Cover shift:** `point − cover` converted to twips with truncation, exactly as MATHSPIKES 1 does.
   This keeps the spike glitch identical.
5. **Local point:** `(ia·dx + ic·dy)/20`, `(ib·dx + id·dy)/20` with no rounding. Then the same strict
   `PointInTriangle`.

Properties (simulator: `./rbsim test` section 20, and a 15.6 M-state comparison over all spikes):

| | plain spikes | rotated/scaled spikes |
|---|---|---|
| MATHSPIKES 2 vs MATHSPIKES 1 (reference window), random grazing states | **0 / 15.6 M differ** | 0.2–0.45 % differ (exactly the twip-rounding cases) |
| depends on window offset / scale / zoom / camera | no | no |

Existing logs and routes on plain-spike levels (3, 6, 7, 8, 12, 13, 14, 16) are therefore unaffected.
On 9, 10, 11, 15 and 17, a route that relied on a lucky rounding at the reference resolution may need
re-checking.

**Simulator:** `./rbsim <cmd> --mathspikes 2 ...` (search, verify, run, calib). `verify` also switches
automatically when a log's `# config` line says `mathspikes 2`. The C++ (`BallHitsSpikeMath` in
`src/redball.cpp`) mirrors the AS3 operation for operation, so MATHSPIKES 2 decisions should match Flash
bit for bit. **This still needs confirming with one sweep (§4, step 3).**

`display_data.h` now also carries each spike's raw matrix chain (`SpikeObj::n`, `chain[]`). Regenerate
it with JPEXS FFDec **18.x** (`-swf2xml`); newer FFDec versions write matrix scales as rounded decimals,
which `tools/swf_geom.py` cannot read exactly.

## 4. Experiments (the existing calibration key `]`, no new mod code needed)

Optionally, add this line to `dbgLevelCalib()` in the mod, right after `L = this.m_currLevel;`. It records
the window transform Flash really uses; the `L.transform.concatenatedMatrix` part is what decides
between A and B.

```as3
var envSt:* = Preloader.stageLink; var envM:Matrix = L.transform.concatenatedMatrix;
var envP:Point = L.localToGlobal(new Point(0,0));
out.push(["#ENV",Capabilities.version,Capabilities.playerType,envSt.stageWidth,envSt.stageHeight,envSt.align,
   envSt.scaleMode,envSt.quality,envSt.displayState,hex(this.x),hex(this.y),hex(this.scaleX),hex(this.scaleY),
   hex(envM.a),hex(envM.b),hex(envM.c),hex(envM.d),hex(envM.tx),hex(envM.ty),hex(envP.x),hex(envP.y),
   CONFIG.MATHSPIKES,int(this.isGless)].join("\t"));
```

`rbsim calib` prints the `#ENV` line together with the model it is checking against.

1. **MATHSPIKES 1, showAll, two or three window sizes** (for example 813 px wide and 939 px tall, and
   one more). Press `]` on **Level 10** (4 of the 5 old near-ties) and **Level 3** (plain spikes,
   E8c-style). Name the files with the size, e.g. `rb1_calib_L10_w813.tsv`.
2. Run `./rbsim calib rb1_calib_L10_w813.tsv --screen-offset X Y` and
   `... --stage-scale S [--window-variant 0|1]` to fit the window model. Level 3 should come out
   identical to the reference under A and change under B.
3. **MATHSPIKES 2** (`/config mathspikes 2` in the patched SWF): the same `]` dumps at two sizes. They
   must be **identical to each other** and give `CALIB OK` under `./rbsim calib <file> --mathspikes 2`.

## 5. Note on the Level 11 string from the chat

`d23a14S1a1q11d4e2d21e1d15S1d9S7e23d31e1d37e37W2e3d48e43d10` runs under the **pixel-hitbox real game**,
not MATHSPIKES. Under MATHSPIKES 1 in the simulator it dies on tick 166, on the 180°-turned ceiling row
near x ≈ −2209. That is a *plain* spike, hit 3 twips inside the bounds and 3.6 twips into the shifted
triangle. No twip-level rounding can save it, which is consistent with it relying on the real game's
pixel hitboxes. With spikes off it wins on tick 345.
