# rbsim — bit-exact Red Ball 1 simulator (milestone 1: core engine)

A C++17 port of the Box2DFlash 2.0.x engine embedded in *Red Ball 1*
(practice-hack / tournament SWFs; their Box2D code is byte-identical), plus
the game-side control logic, built as the foundation for brute-force route
search.

## Build & run

```
make              # g++ -O2 -ffp-contract=off (no FMA), strict IEEE doubles
./rbsim test      # self-tests: resting, rolling, jumping, determinism, snapshots
./rbsim run --inputs "n20w1d25n40" [--hex] [--every N]
./rbsim bench     # ~0.9M frames/s/thread on Level 1 incl. snapshot restores
./rbsim verify rb1_stats.tsv   # bit-exact comparison against a Flash stats log
```

`run` prints the player's position, velocity, angle, angular velocity, ground
probes (L/C/R), contact-listener count, alive and sleep flags per frame. With
`--hex`, it also prints the raw IEEE-754 bits for exact comparison against the
TAS stat logs. Inputs use the game's own RLE format (`n w d e a q S W R`); the
legacy digit format is also accepted.

## Architecture

| file | contents |
|---|---|
| `b2math.h` | AS3-exact vector/matrix/sweep math; `Number.MIN_VALUE` epsilons; the single `as3_sin/as3_cos` choke point |
| `b2collision.cpp` | polygon geometry (normals, centroid, OBB, core vertices), circle/poly narrow phase, GJK distance, TOI |
| `b2world.cpp` | SAP broadphase, pair manager, contact lifecycle and listener emulation, island + contact solver, TOI loop, bodies/shapes |
| `redball.cpp` | Level/PlayerBox construction order, ground probes, input nudges and forces, death line, RLE codec |
| `rbsim.cpp` | CLI, self-tests, benchmark |
| `verify.cpp` | replays `rb1_stats.tsv` logs and compares every field bit-for-bit |
| `tools/trig_flip_search.cpp` | diagnoses divergences caused by 1-ulp `sin`/`cos` differences |

- **Snapshots:** all mutable state lives in fixed arrays linked by `int`
  indices, so `Sim copy = sim;` is a complete, bit-exact snapshot (128 KB now,
  easy to shrink).
- **Shared geometry:** immutable polygon data lives in a `GeomTable` that
  snapshots share.
- **Scratch memory:** island and constraint buffers are thread-local, which
  makes the engine safe to run one-sim-per-thread in parallel.

## Port quirks replicated (differences from stock Box2D 2.0 C++)

1. Pair buffer is **not sorted** before commit, so contacts are created in
   insertion order, which drives solver order.
2. Contact velocity bias for separated points is a hard-coded `-60 * separation`.
3. The friction clamp uses the normal impulse from **before** the current
   iteration's update.
4. Restitution bias is computed from pre-warm-start velocities.
5. `Number.MIN_VALUE` (a denormal) is used everywhere C++ used `FLT_EPSILON`.
6. Uninitialised `Number` fields are **NaN** (manifold impulses/separation,
   `m_toi`, TOI sub-step fields).
7. Bodies are created static, so `SetMassFromShapes` flips their type and
   `RefilterProxy` destroys and recreates proxies (new ids, new pair order).
8. Game listener bug: `playerContactBodies.splice(body, 1)` always removes
   index 0. Add/Remove also fire per manifold point and on every feature-key
   change, so the list can hold stale or duplicate entries.
9. Destroyed bodies remain readable (the game keeps probing a dead ball).
10. Decompiler loss: JPEXS dropped the `bestEdge/bestSeparation` update in
    `FindMaxSeparation`; the AVM2 p-code confirms it exists and the port
    includes it.

## Verification status (Flash Player 11.5.502.149, Windows 8, x86)

Checked against a real stats log (`docs/STATS_LOGGING.md` describes the mod):
388 fresh Level 1 runs, 113,938 frames, every field compared bit-for-bit.

- **376 / 388 runs are bit-exact** from level load to the end of the run,
  death or win. This covers the broadphase and pair order, solver quirks, the
  listener bug, TOI, sleeping, ground probes, the contact list, twips and
  sprite rotation.
- The remaining 12 runs diverge only because Flash's `Math.sin` / `Math.cos`
  differ from glibc by 1 ulp in about 3.5% of inputs.
  `tools/trig_flip_search` proves it: with four 1-ulp `sin` corrections the
  full 1,528-tick Level 1 TAS matches through the win.
- Without the corrections the TAS drifts: visible at tick 510, and the ball
  misses a jump by tick 750. **Exact Flash trig is required.**

Calibration results (`rb1_calib.tsv`):

| question | answer |
|---|---|
| twip quantisation of `x`/`y` | truncation toward zero (2,400/2,400) |
| `rotation` getter | written value, `fmod`-normalised to (-180, 180] (720/720) |
| x87 extended precision in physics | no; all arithmetic matches SSE2 doubles |
| `Math.sin`/`cos` | **not** glibc, x87 `fsin`/`fcos`, fdlibm, or Intel LIBM x64. ~0.52-ulp error signature, likely the MSVC 32-bit CRT SSE2 routines statically linked into the player. **Open.** |

Tools:

```
./rbsim verify rb1_stats.tsv [--verbose N] [--ignore sr,...]
make tools/trig_flip_search && tools/trig_flip_search rb1_stats.tsv <LEVEL line>
```

## Roadmap

1. Implement Flash's exact `sin`/`cos`, extracted from the Flash Player binary, and re-verify (target: 388/388).
2. Joints: revolute, prismatic, distance, mouse (structure already wired;
   `Solve` aborts if a joint is present).
3. Automatic extraction of all 17 levels from the SWF (placements, rotations,
   per-level `Update` logic: moving platforms, motors, gates).
4. Display layer: goal/checkpoint `hitTestObject` with the rotated ball bbox,
   spike hit-points, Level 15 kill line.
5. Search: shrink snapshots, add a work-stealing thread pool, prefix-shared
   branching.
