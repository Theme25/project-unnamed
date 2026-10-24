# MATHSPIKES and the Flash window resolution

**Short version.**
- **MATHSPIKES 1** (the Practice Hack's standardized spikes, the speedrun rule) **does not depend on the window**.
  Its `localToGlobal` / `globalToLocal` work in stage twips, and the window matrix never enters. rbsim's model
  matches dumps recorded at five window sizes (the team's 12 `rb1_calib*_spikes1/2` dumps, plus E8c/E11 sweeps).
- **MATHSPIKES 2** (§3) is the same test in Level coordinates with plain Numbers. It is identical to MATHSPIKES 1
  on plain spikes and independent of everything display-related.
- **MATHSPIKES 0** (the original game and the Tournament Edition: `hitTestPoint(x, y, true)`) **does** depend on
  the window size, the scale mode, the camera and the previous frame's redraw. rbsim models it bit-exactly:
  `--mathspikes 0 --window WxH [--noscale]`. See **`docs/RESOLUTION.md`**.

Earlier versions of this document proposed two hypotheses ("A": a whole-twip letterbox offset; "B": a showAll scale
in every conversion) for how the window could change MATHSPIKES 1. Both are obsolete. The decompiled player shows
that `localToGlobal` / `globalToLocal` never include the window, and the five-size dumps confirm it. The
`--screen-offset` / `--stage-scale` options are gone. Inconsistencies seen across resolutions come from the
real game's shape test (MATHSPIKES 0 / Tournament Edition), not from MATHSPIKES 1.

## 1. Where twip rounding still matters in MATHSPIKES 1

`Shipik.testPoint(globalPoint)` (decompiled):

```as3
point = this.p.globalToLocal(point);                                   // (1) stage -> Level
if (!this.getBounds(this.p).containsPoint(point)) return false;        // (2)
var pt:Point = globalToLocal(this.p.localToGlobal(point.subtract(cover))); // (3) Level -> stage -> Shipik
return PointInTriangle(pt);
```

For **rotated or scaled spikes** (Levels 9, 10, 11, 15, 17) the inverse transforms round to whole twips in
**stage** coordinates. Stage coordinates include the camera but not the window. A grazing contact can therefore
depend on the camera position. rbsim reproduces this exactly (float32 inverse ported from the player, E11
390,670 / 390,670; `docs/FLASH_PLAYER_RE.md` §4). For plain spikes every rounding is lossless.

## 2. Measured (the team's dumps and the projector)

| dumps | window sizes | result |
|---|---|---|
| `rb1_calib3.{1,2,3}_spikes{1,2}.tsv` (E8c, Level 3) | 3 per rule | 30,000 / 30,000 each, MATHSPIKES 1 and 2 |
| `rb1_calib_L10.{1,2,3}_spikes{1,2}.tsv` (E11, Level 10) | 3 per rule | 154,396 / 154,396 each |

The hit counts differ between MATHSPIKES 1 and 2 on Level 10 (55,102 vs 55,318), as §3 predicts for turned
spikes. Within a rule they do not change with the window.

## 3. MATHSPIKES 2

Files: `docs/mathspikes2/Shipik.as`, `docs/mathspikes2/PlayerBox.as` (the original classes plus the new
branch, plus `envLine` for the `#ENV` line, §4). In the ready-made SWF only these two classes are
recompiled. `Game` only gets the three `#ENV` calls inserted as bytecode, and every other method is
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

**Simulator:** `./rbsim <cmd> --mathspikes 2 ...` (smartforce, verify, run, calib). `verify` also switches
automatically when a log's `# config` line says `mathspikes 2`. The C++ (`BallHitsSpikeMath` in
`src/redball.cpp`) mirrors the AS3 operation for operation, so MATHSPIKES 2 decisions should match Flash
bit for bit. Confirmed by the team's MATHSPIKES 2 dumps (§2).

`display_data.h` now also carries each spike's raw matrix chain (`SpikeObj::n`, `chain[]`). Regenerate
it with JPEXS FFDec **18.x** (`-swf2xml`); newer FFDec versions write matrix scales as rounded decimals,
which `tools/swf_geom.py` cannot read exactly.

## 4. Experiments

### The resolution is written into every dump

`Shipik.envLine` (in `docs/mathspikes2/Shipik.as`) builds a `#ENV` line. `tools/abc_patch_env.py` patches
the mod's `Game` bytecode so the line is written at the top of each dump. `Game` is **not** recompiled:
the tool inserts one call into each of three methods and moves nothing else.

| key | dump | `#ENV` written |
|---|---|---|
| `]` | `rb1_calib_L<id>.tsv` (E9/E10/E11/E12) | after the level is loaded |
| Shift+`]` on Level 3 | `rb1_calib.tsv` (E8 plain-spike dump) | at the start of the E8 block |
| `[` | `rb1_stats.tsv` (stats log) | first line (window at **export** time) |

The line is key/value pairs, tab separated:

```
#ENV  window 813x1032  scaleMode showAll  align   stageSize 550x400  quality HIGH  displayState normal
      screen 1920x1080  player WIN 11,4,402,287  playerType StandAlone  mathspikes 1
      rootMatrix a,b,c,d,tx,ty  levelMatrix a,b,c,d,tx,ty  levelOrigin x,y  levelXY x,y   (matrices as hex doubles)
```

- `window` is the real window client size. Under showAll `stage.stageWidth` stays 550, so `envLine`
  switches to noScale for an instant to read the window size, then restores the mode in the same call.
- `rootMatrix` / `levelMatrix` are `transform.concatenatedMatrix` (the stage-to-root and Level matrices).
- `rbsim calib` prints the line. `rbsim verify` takes `mathspikes`, `window` and `scaleMode` from it. A log
  recorded with `/config mathspikes 0` or `2` is checked under that rule and window automatically. `rbview` does
  the same.

To rebuild the SWF from a fresh Practice Hack:
1. Compile `docs/mathspikes2/Shipik.as` and `PlayerBox.as` in with FFDec:
   `ffdec -replace in.swf mid.swf Shipik Shipik.as PlayerBox PlayerBox.as`.
2. Run `python3 tools/abc_patch_env.py mid.swf out.swf`.

### What to record

The window question is settled (§2 and `docs/RESOLUTION.md`). Recording MATHSPIKES 0 playbacks at several window
sizes is described in `docs/RESOLUTION.md` §5.

## 5. Note on the Level 11 string from the chat

`d23a14S1a1q11d4e2d21e1d15S1d9S7e23d31e1d37e37W2e3d48e43d10` runs under the **pixel-hitbox real game**,
not MATHSPIKES. Under MATHSPIKES 1 in the simulator it dies on tick 166, on the 180°-turned ceiling row
near x ≈ −2209. That is a *plain* spike, hit 3 twips inside the bounds and 3.6 twips into the shifted
triangle. No twip-level rounding can save it, which is consistent with it relying on the real game's
pixel hitboxes. With spikes off it wins on tick 345.
