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

## Unverified assumptions (need the TAS stat logs)

- **Flash `Math.sin/cos` vs glibc.** Only affects rotating bodies (the ball's
  position never depends on its angle), so Level 1 is immune.
- **Twip quantisation of `DisplayObject.x/y`** (truncate vs round). This only
  matters for display-driven checks: the death line, goal/checkpoint
  `hitTestObject`, and spikes.
- **Flash JIT precision:** SSE2 doubles are assumed. If logs diverge on the
  very first frames, x87 extended precision is the suspect.

## Roadmap

1. Verify against stat-augmented TAS strings (first divergence frame + field).
2. Joints: revolute, prismatic, distance, mouse (structure already wired;
   `Solve` aborts if a joint is present).
3. Automatic extraction of all 17 levels from the SWF (placements, rotations,
   per-level `Update` logic: moving platforms, motors, gates).
4. Display layer: goal/checkpoint `hitTestObject` with the rotated ball bbox,
   spike hit-points, Level 15 kill line.
5. Search: shrink snapshots, add a work-stealing thread pool, prefix-shared
   branching.
