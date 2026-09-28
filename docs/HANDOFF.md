# rbsim — project handoff (continue here in a new chat)

This document is self-contained. It records everything established so far
while building a bit-exact simulator of *Red Ball 1* (Flash), so work can
continue on the remaining levels in a fresh conversation.

---

## 0. How to start the next chat

Upload these files:

| file | why |
|---|---|
| this `HANDOFF.md` | context |
| `Red_Ball_-_Practice_Hack.swf` | the game (the source of truth) |
| `Red_Ball_-_Tournament_Edition.swf` | optional; physics-identical to the practice build |
| any new `rb1_stats*.tsv` / `rb1_calib*.tsv` logs | verification data |

Then say something like:

> Read HANDOFF.md. Clone https://github.com/Theme25/project-unnamed, set up the
> environment (section 9), run `make && ./rbsim test`, then continue with level N.

The repo is **public**. The assistant can clone it but cannot push, so the user
pushes by pulling a git bundle into an up-to-date clone (section 10).

---

## 1. Goal

Brute-force the fastest *Red Ball 1* speedrun routes. That requires a
simulator that reproduces the Flash game **bit-for-bit**: same IEEE-754
doubles, same contact order, same quirks. Routes found offline must replay
identically in the real game.

- Target player: **standard Flash Player 11** (tested 11.4.402.287, Windows
  XP, x86). Its `Math.sin`/`cos` are reproduced exactly.
- A custom ActiveX host (Flash 11.5.502.149) uses a *different*, still
  unidentified `sin`/`cos` (1-ulp differences on ~3.5% of inputs). It is **not**
  the target unless the user decides otherwise.
- Language: **C++17** (user's choice). Search design: parallel, one sim per
  thread, pointer-free memcpy-able snapshots with prefix sharing. GPU/SIMD
  batching was rejected: control flow diverges and GPUs can't guarantee
  determinism.

---

## 2. Current status

| level | status |
|---|---|
| 1 | **bit-exact** against Flash logs (FP 11.4: 2/2 runs, 1,204/1,204 frames, incl. TAS win at tick 510) |
| 2 | implemented (pendulum = distance joint, moving platform = prismatic joint); self-tests pass; **awaiting Flash logs** |
| 3–17 | placements extracted (`src/levels_data.h`); scripts not written |

What is done:
- **Engine:** the full Box2DFlash 2.0.x engine: broadphase, pair manager,
  contacts, contact solver, TOI, sleeping, distance/prismatic/revolute joints.
- **Trig:** exact Flash Player 11 `sin`/`cos` (Intel LIBM, `src/libm_intel.S`).
- **Tools:**
  - `rbsim verify <log.tsv>` compares every field bit-for-bit
  - `rbsim log` writes the simulator's log in the Flash mod's format
  - `tools/trig_flip_search` diagnoses divergences caused by 1-ulp trig differences
- **Test data:**
  - ActiveX log: 388 Level 1 runs, 113,938 frames; 376 runs bit-exact with its
    unknown trig, the rest differ only through trig.
  - FP 11.4 log: 2 runs, 100% bit-exact.

What is not done:
1. Level 2 verification against Flash logs.
2. **The display layer:**
   - the goal/checkpoint `hitTestObject` tests (rotated ball bounding box)
   - spikes (`Shipik` hit points)
   - `DisplayObject.rotation` derived from timeline matrices (needed by
     levels 3, 4, 8–13, 15, 16)
3. Level scripts 3–17.
4. The search itself: snapshot shrinking, thread pool, prefix-shared
   branching, pruning heuristics.

---

## 3. Repository layout (`Theme25/project-unnamed`)

| path | contents |
|---|---|
| `src/b2math.h` | AS3-exact math: `Vec2`/`Mat22`/`XForm`/`Sweep`; `Number.MIN_VALUE` epsilons; `as3_sin/cos` choke point (`TrigImpl::IntelLibm` default, `Glibc` optional, diagnostic hooks) |
| `src/b2world.h` | pointer-free state: fixed arrays linked by `int` indices; `World`, `Body`, `Shape`, `Contact`, `Joint`, broadphase, pair manager, listener emulation |
| `src/b2collision.cpp` | geometry (`GeomTable`), narrow phase, GJK, TOI |
| `src/b2world.cpp` | broadphase / pair manager / contacts / island / contact solver / `World::Step` |
| `src/b2joints.cpp` | distance, prismatic, revolute joints; `CreateJoint`/`DestroyJoint` |
| `src/libm_intel.S` | Flash Player 11 `sin`/`cos` (**GPL-2.0-only**, derived from OpenJDK/Intel) |
| `src/levels_data.h` | named placements of all 17 levels (generated) |
| `src/redball.h/.cpp` | game layer: base `Level` logic, level scripts, sprite state, RLE codec |
| `src/rbsim.cpp` | CLI: `run`, `log`, `test`, `bench`, `verify` |
| `src/verify.cpp` | log replay and comparison |
| `tools/hotspot2gas.py` | OpenJDK `MacroAssembler` stub → GAS translator (regenerates `libm_intel.S`) |
| `tools/extract_levels.py`, `tools/gen_levels_data.py` | SWF XML → `levels.json` → `levels_data.h` |
| `tools/trig_flip_search.cpp` | greedy ±1-ulp `sin`/`cos` override search |
| `docs/STATS_LOGGING.md` | how to mod the SWF to produce verification logs |

Build: `make` (Linux x86-64: g++ with `-ffp-contract=off`, SSE2 doubles, no
fast-math). `libm_intel.S` is System V x86-64 only; a Windows build needs a
calling-convention adaptation. The user is on Windows and uses Codespaces or WSL.

---

## 4. Key facts about the game

### 4.1 Physics setup (`Levels/Level.as`)

**World:**
- AABB ±1000, gravity (0, 10), `allowSleep` true.
- `positionCorrection`, `warmStarting` and `continuousPhysics` are all true.
- Step: `m_world.Step(1/30, 10)` once per frame.
- Scale: 30 px per metre.

**Construction order:**
1. World.
2. Ground body.
3. Contact listener.
4. `PlayerBox`, at `checkPoints[lastCheckNum]`: CreateBody → SetBullet(true)
   → CreateShape → SetMassFromShapes.
5. The `Level_N` constructor's `CreateBody` calls, in order.
6. The `Level_N` constructor's joints.

**Ball:**
- Radius `21/30/2`; the sprite bounds are 420 twips.
- Density 1, friction 0.4, restitution 0.2.

**`Level.CreateBody(name, kind, density, friction, restitution, data)`:**
- Body position is `sprite.x/30, sprite.y/30`; angle is `sprite.rotation*(PI/180)`.
- `"Polygon"`/`"BluePolygon"`: `data` is a list of polygons in px, and each
  vertex is divided by 30.
- `"Circle"`: `data = [size]`, giving radius `size/30/2` and **localPosition
  (r, r)**.
- Ends with `SetMassFromShapes`.
- Defaults: density 1, friction 1, restitution 0.2.

**Checkpoints:** `checkPoint0..4` are collected until the first missing one.
`lastCheckNum` is static, so it survives restarts (`R`).

### 4.2 Per-frame logic (`Game.UpdateHandler` → `Level.Update`)

1. `m_world.Step(1/30, 10)`.
2. Sprite sync, for every non-static body whose `userData` is a Sprite:
   - `x = pos.x*30`, `y = pos.y*30`, **truncated to twips** (toward zero).
   - `rotation = angle*(180/PI) % 360`; the getter returns it
     `fmod`-normalised to (−180, 180].
3. Ground probes: `GetBodyAtPoint(x, y+0.4)`, `(x−0.2, y+0.38)`,
   `(x+0.2, y+0.38)`, static bodies included.
   - Each is a world query with an AABB of ±0.001 (max 10 results), followed
     by `TestPoint`.
   - The player counts as grounded if any probe hits.
4. Left:
   - grounded: if `vx > −5` then `vx −= 0.5`;
   - airborne: if `vx > −2.5` then `vx −= 0.25`.

   Right is symmetric. Velocity is read live, so pressing L and R on the same
   frame compounds.
5. Up:
   - if `playerContactBodies.length > 0 && grounded`: `ApplyForce((0,−65), worldCenter)`;
   - else if `vy < 0`: `ApplyForce((0,−1))`.
6. The win check (`levelAim.hitTestObject`) and spikes. **Not implemented.**
7. The level's own `Level_N.Update` runs after `super.Update`: death line
   (`playerBox.y > 550 && IsLive()`), moving parts, and so on.

**Input string (RLE) format:**
- Letters map to input codes: `a=4 w=2 d=1 q=6 e=3 S=5 W=7 n=0 R=8`.
- Each code is `4·L + 2·U + R`.
- `R` restarts from the current checkpoint and does not consume a tick; its
  count is ignored.
- A string whose first char code is below 64 uses the legacy one-digit-per-frame
  format.

**Win:** `PlayerWin` sets `isTimeStop`, `tPause = 0` (the game stops ticking),
and linear/angular damping 3.

**Death:** `PlayerDie` spawns 8 debris bodies using `Math.random()`. The sim
treats death as terminal. The practice hack has an invincibility toggle.

### 4.3 Engine quirks replicated (differ from stock Box2D 2.0 C++)

1. The pair buffer is **not sorted** in `Commit`, so contacts are created in
   insertion order.
2. Contact velocity bias for separated points is a hard-coded `−60·separation`.
3. The friction clamp uses the normal impulse from **before** the current
   iteration's update.
4. The restitution bias uses pre-warm-start velocities.
5. `Number.MIN_VALUE` (a denormal) is used where C++ used `FLT_EPSILON`.
6. Uninitialized `Number` fields are **NaN**; `int` fields are 0.
7. Bodies are created static, so the type flip in `SetMassFromShapes` makes
   `RefilterProxy` recreate proxies (new ids, new pair order).
8. `MyContactListener.Remove`: `splice(body, 1)` always removes index 0.
   Add/Remove fire per manifold point and on every feature-key change.
9. Destroyed bodies stay readable; the game keeps probing a dead ball.
10. Joints use the 2.0.x **force** formulation (`m_force`, `dt·force`). In the
    island solver, contacts are solved before joints, and every joint's
    position solve is always evaluated.
11. `CreateJoint`/`DestroyJoint` refilter the shapes of the body with fewer
    shapes (ties go to body2).
12. The prismatic joint's position solver, in the equal-limits case, uses
    `b2Max(linearError, |angularC|)`, as in the AS3.
13. `SetLinearVelocity` does not wake the body.

### 4.4 Decompiler warning

JPEXS silently **dropped statements** in `b2Collision.FindMaxSeparation`
(the bestEdge/bestSeparation update); the p-code confirmed they exist. So:
- **the p-code is authoritative;**
- for every newly ported class, cross-check field writes (`setproperty`
  counts in the p-code vs assignments in the decompiled source). All 48 joint
  methods passed this check.
- never recompile `Box2D.*` classes when modding the SWF.

---

## 5. Flash environment facts (from calibration dumps)

| item | result |
|---|---|
| twips for `x`/`y` | truncation toward zero (2,400/2,400 on both players) |
| `rotation` getter | written value, `fmod`-normalised to (−180, 180] (720/720) |
| arithmetic | plain IEEE doubles (SSE2); no x87 extended precision |
| `Math.sin`/`cos`, FP 11.4 | **Intel LIBM SSE2**, identical to Java `Math.sin`/`cos` on x86-64 (0/4,096 mismatches); ported via `tools/hotspot2gas.py` (0/83,360 vs Java) |
| `Math.atan2`, FP 11.4 | x87 `fpatan` (0/4,096); needed later for matrix→rotation |
| ActiveX 11.5 `sin`/`cos` | unidentified: not glibc, x87, fdlibm, or Intel LIBM x64 |

Why trig matters: `Mat22.Set(angle)` feeds contact anchors and joint lever
arms. Without exact trig the Level 1 TAS diverges at tick ~300, is visibly off
by tick 510, and misses a jump by tick 750.

---

## 6. Verification workflow

1. The user mods the SWF following `docs/STATS_LOGGING.md`.
   - It logs, per frame: tick, frame, input, position, velocity, angle,
     angular velocity, sleep time, flags, the 3 probes, the contact-list
     length and names, the world contact count, and sprite x/y/rotation.
   - All doubles are logged as big-endian hex.
   - §3.4 adds **extra-body groups** (`name px py a vx vy w`) for every other
     dynamic body.
   - `LEVEL <id> <checkpoint>` lines separate runs; `R` lines mark restarts.
2. Run `./rbsim verify log.tsv [--verbose N] [--trig intel|glibc]`. For each
   run it reports the first divergent tick and field.
3. If a divergence looks like tiny noise (~1e-16), run
   `tools/trig_flip_search log.tsv <LEVEL line>` to test the trig hypothesis.
4. Reference output: `./rbsim log --level N --inputs "<rle>"` produces the
   simulator's log in the same format.

Level 1 TAS (inputs), which wins at tick 510 on FP 11.4:
```
d18e13d2e7d15a1n1a1n1e8q73e8w1e5d18a14d1e1n8a10q6a11q9W8e20a1n17d8e12d9a7d17e1q3e8d1e1d26a19n2a5n1a10d2w5n3a8q3a36d13n1d15n2d3a1e1d8
```

---

## 7. Adding a level (recipe)

1. Read `Levels/Level_N.as`, both the constructor and `Update`.
2. Transliterate the constructor into a `LN_Construct(Sim&)` in
   `src/redball.cpp`:
   - use `s.CreateBody(name, "Polygon", density, friction, restitution, polys)`
     or `s.CreateCircleBody(...)`, **in the same order** as the AS3;
   - build joints with `w.InitDistanceJointDef` / `InitPrismaticJointDef` /
     `InitRevoluteJointDef`, set the motor/limit fields exactly, then call
     `w.CreateJoint` in order;
   - keep private fields in `s.lvBody[]` / `s.lvInt[]`; add more `Sim` fields
     if a level needs doubles.
3. Transliterate `Update` into `LN_Update(Sim&)`. Use `s.spriteX/Y/Rot[body]`
   wherever the AS3 reads a sprite's `x`/`y`/`rotation`, because those are
   twip-quantised. Anything the AS3 calls on bodies must be mirrored exactly,
   e.g. `SetLinearVelocity` doesn't wake bodies, while `ApplyForce` and
   `ApplyImpulse` do.
4. Register the level in `GetLevelScript()`.
5. Handle rotated placements: 10 levels need `DisplayObject.rotation` from a
   timeline matrix. The current code aborts on non-identity matrices; see
   section 8.
6. Add self-tests to `rbsim test`, then request Flash logs with extra-body
   columns and run `verify`.
7. Check the level's AS3 for other `Box2D` API use: `DestroyBody`,
   `SetXForm`, filters, `ApplyImpulse`, `m_linearDamping`, and so on.
   `World` has `DestroyBody`, `ApplyImpulse`, `SetLinearVelocity` and
   `Refilter`; add the rest faithfully from the AS3 or p-code.

### Joint usage per level (`CreateJoint` count; `enableMotor`/`enableLimit` occurrences)

| level | joints |
|---|---|
| 2 | distance ×1, prismatic ×1 (mouse def created but never added) |
| 3 | prismatic |
| 4 | prismatic, revolute (5 CreateJoint) |
| 5 | none |
| 6 | revolute ×4, motors |
| 7 | distance, prismatic, motors + limits (7 CreateJoint) |
| 8 | prismatic, revolute (5) |
| 9 | distance, prismatic, revolute, motor (19 CreateJoint: chains) |
| 10 | prismatic, revolute, motors ×7, limits ×3 |
| 11 | distance, revolute, motors ×4 (8) |
| 12 | distance, prismatic, revolute (4) |
| 13 | distance, prismatic (4) |
| 14 | prismatic, revolute (2) |
| 15 | prismatic, revolute (4) |
| 16 | prismatic, revolute, motor + limit (4) |
| 17 | prismatic, revolute, motor + limit (4) |

Only distance, prismatic and revolute joints are ever created; all three are
implemented. Rotated or scaled placements occur in levels 3, 4, 8, 9, 10, 11,
12, 13, 15 and 16.

Known level-specific notes:
- **Level 7:** `redCheck` flags survive `SetLevel(id, true)`.
- **Level 15:** there is a kill line. Its coin tween is time-based in one
  build and frame-based in the other, which is cosmetic only.
- **Level 2:** `movePlatform` is driven by `SetLinearVelocity(2·dir, 0)`,
  reversing when its sprite x is below 390 or above 550.

---

## 8. Open problems / next milestones

1. **Level 2 logs** (with §3.4 extras): L2-idle `n900`, the L2 TAS, manual
   runs that hit the pendulum and ride the platform, and a checkpoint-1 restart.
2. **Matrix → `rotation`** for rotated placements:
   - Flash derives `rotation` from the matrix, probably
     `atan2(b, a)·180/π` with the 16.16 fixed-point values.
   - FP 11.4's `atan2` is x87 `fpatan`, which can be reproduced with inline
     asm or an exact port.
   - Settle it with a calibration dump: place rotated test sprites in the
     SWF, or read `rotation` of each level's rotated instances directly, and
     log the hex values.
3. **Display layer** for win and checkpoints:
   - `hitTestObject` compares global bounding boxes. The ball's box depends on
     its rotation and on transformed shape bounds in twips.
   - The camera scaling (`scaleTimer`) could matter through twip rounding.
   - Spikes use `hitTestPoint` shape tests on `Shipik` instances.
   - Needed so the search can detect a finish without Flash.
4. **Search:**
   - Shrink the snapshot (now ~152 KB; copy only the used prefixes of arrays).
   - Work-stealing thread pool, prefix sharing, pruning (e.g. the practice
     build's `distToGoal`).
5. Optionally, identify the ActiveX host's `sin`/`cos`; that needs its binary.

---

## 9. Environment setup (fresh sandbox)

```bash
# repo
git clone https://github.com/Theme25/project-unnamed.git && cd project-unnamed
make && ./rbsim test          # expect ALL PASSED

# JPEXS FFDec (decompiler); Java is usually preinstalled
mkdir -p ~/tools/ffdec && cd ~/tools/ffdec
# download the ffdec_*.zip release asset from github.com/jindrapetrik/jpexs-decompiler/releases
# (version 15.x was used), unzip it, then:
java -jar ffdec.jar -cli -export script ~/extracted/practice ~/uploads/Red_Ball_-_Practice_Hack.swf
java -jar ffdec.jar -cli -swf2xml ~/uploads/Red_Ball_-_Practice_Hack.swf ~/extracted/practice_full.xml
# p-code of one class (authoritative when decompiled code looks odd):
java -jar ffdec.jar -cli -format script:pcode -selectclass Box2D.Collision.b2Collision \
     -export script ~/extracted/pcode ~/uploads/Red_Ball_-_Practice_Hack.swf

# regenerate level placements from the SWF XML
make levels SWFXML=~/extracted/practice_full.xml
```

Useful symbol ids: `Levels.Level_1` is sprite 401, `Level_2` 355, `Level_3`
575, `Level_4` 561, `Level_5` 125, `Level_6` 434, `Level_7` 287, `Level_8`
389, `Level_9` 248, `Level_10` 205, `Level_11` 87, `Level_12` 164,
`Level_13` 601, `Level_14` 536, `Level_15` 474, `Level_16` 328, `Level_17` 493,
and `PlayerBox` 23.

Regenerating `src/libm_intel.S`: download
`stubGenerator_x86_64_{sin,cos,constants}.cpp` from
`raw.githubusercontent.com/openjdk/jdk/master/src/hotspot/cpu/x86/`, then run
`make libm OPENJDK=<dir>`.

---

## 10. Delivering changes to the user

The assistant cannot push. Its workflow:
1. Clone the repo.
2. Commit with the user's identity: `Mohamad Shaikh Khalil
   <Mohamad.sk.work@gmail.com>`.
3. Create an **incremental** bundle based on the current remote head:
   `git bundle create rbsim-update.bundle <remote-head>..main`.

The user then runs, in an up-to-date clone:
```
git pull rbsim-update.bundle main
git push
```
If git reports "Repository lacks these prerequisite commits", the clone is
behind: run `git fetch origin && git reset --hard origin/main` first, or
re-clone.

---

## 11. Licensing note

`src/libm_intel.S` derives from OpenJDK HotSpot code (Intel copyright,
GPL-2.0-only), so any rbsim binary that includes it is GPLv2. The rest of the
project is the user's own code and currently has no license chosen.
