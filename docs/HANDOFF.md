# rbsim — project handoff (continue here in a new chat)

rbsim is a bit-exact C++ re-implementation of the Flash game *Red Ball 1* (Box2DFlash 2.0.x
physics + the game's own logic), built so a speedrunning team can brute-force faster routes.
"Bit-exact" means every logged value of every frame matches Flash Player 11.4 to the last bit.

## 0. How to start the next chat

Upload to the new chat:

| file | why |
|---|---|
| this file (`docs/HANDOFF.md`) | context |
| `Red_Ball_-_Practice_Hack.swf` | the game: the source of truth for every level script and placement |
| any new `rb1_stats_*.tsv` / `rb1_calib_*.tsv` logs | verification data for the work at hand |

Then say, for example:

> Read HANDOFF.md. Clone https://github.com/Theme25/project-unnamed, set up the environment
> (section 9), run `make && ./rbsim test`, then continue with <task>.

The repo is **public**. The assistant can clone it but cannot push; the user pushes by pulling a
git bundle (section 10). Logs from earlier chats are **not** in the repo; earlier results are
recorded in README.md ("Verification status") and below.

## 1. Who and what

- The user (Mohamad) coordinates a Red Ball 1 speedrunning community; he records Flash logs with
  a modded SWF and manages the team's machines himself (most have 32 GB RAM, some 16 GB).
- **Reference setup:** Flash Player **11.4.402.287** (Windows XP), Practice Hack SWF,
  **`MATHSPIKES = 1`** (standardized spikes, the speedrun rule; now the mod's default) and
  **`isGless` off** (spike glitch on). A custom ActiveX host (Flash 11.5) has different `sin`/`cos`:
  never use its logs.
- The user writes short messages and works level by level. He wants accuracy first, honest
  statements of what is and is not verified, and asks for README/handoff accuracy checks.
- Teammate notes (from the user): see section 5 (death warp, timers, spike glitch).

## 2. Current status (all verified against Flash unless stated)

| area | status |
|---|---|
| physics core | bit-exact (Box2DFlash quirks replicated; Intel LIBM `sin`/`cos`) |
| levels **1-8, 12, 14** | **bit-exact** against Flash logs (48 logs, 59,741 frames; table in README) |
| levels 9, 10, 11, 13, 15, 16, 17 | not scripted (section 8) |
| display layer | exact: rotation matrix, hit tests, camera, standardized spikes (section 6) |
| death / death warp | exact: post-death camera + unguarded goal/checkpoint/switch tests; 3 logged warps verified |
| Windows | MinGW-w64 build; checked under Wine 9: all tests, logs, calibrations identical |
| `rbsim optimize` | local search from a known route (works; does not beat the team's TASes in short runs) |
| `rbsim beam` | beam search from any start, resumable, deterministic; quality limited by its score (section 7) |

Self-tests: `./rbsim test` → 55 tests, `ALL PASSED`.

## 3. Repository layout (`Theme25/project-unnamed`)

| path | contents |
|---|---|
| `src/b2math.h`, `b2collision.cpp`, `b2world.cpp/.h`, `b2joints.cpp` | Box2DFlash 2.0.x port (fixed arrays, `int` links, so `Sim` is trivially copyable) |
| `src/libm_intel.S` | Flash 11.4's `sin`/`cos` (Intel LIBM via OpenJDK, `tools/hotspot2gas.py`); Win64 wrappers under `_WIN32` |
| `src/redball.cpp/.h` | `Sim`: level logic, sprite sync, inputs, camera, display layer, spikes, death, level scripts, RLE codec |
| `src/levels_data.h` | named placements of all 17 levels (generated) |
| `src/display_data.h` | bounds of named objects + every spike, per level (`tools/gen_display_data.py`, `tools/swf_geom.py`) |
| `src/flash_sintab.h` | Flash's display-matrix sine table (`tools/gen_flash_sintab.py`) |
| `src/snapshot.cpp/.h` | compact snapshots (diff vs level start, ~6 KB, byte-exact, portable) |
| `src/verify.cpp` | `rbsim verify`: replays stats logs, compares every field |
| `src/calib.cpp` | `rbsim calib`: checks calibration dumps |
| `src/search.cpp` | `rbsim optimize` |
| `src/beam.cpp` | `rbsim beam` |
| `src/rbsim.cpp` | CLI, self-tests, bench |
| `docs/STATS_LOGGING.md` | the logging mod spec (§3.0 is the current per-level format) and every calibration result |
| `docs/WINDOWS.md` | end-user guide for `rbsim.exe` |
| `README.md` | overview, commands, verification tables (kept accurate; re-check after changes) |

## 4. Key facts about the game

### 4.1 Physics setup (`Levels/Level.as`)

- World AABB ±1000, gravity (0, 10), `allowSleep`; position correction, warm starting and
  continuous physics on. `m_world.Step(1/30, 10)` once per frame. 30 px per metre. 31 fps for
  RTA timing (frames / 31 = seconds).
- Construction: world, ground body, contact listener, `PlayerBox` at `checkPoints[lastCheckNum]`
  (CreateBody, SetBullet, CreateShape, SetMassFromShapes), then the `Level_N` constructor's
  bodies and joints **in AS3 order** (order drives contact/solver order).
- Ball: radius `21/30/2`, density 1, friction 0.4, restitution 0.2; sprite box 420 twips.
- `Level.CreateBody(name, kind, density, friction, restitution, data)`: position `sprite.x/30`,
  angle `sprite.rotation * PI/180` (the **getter**; for timeline-rotated clips Flash's own value,
  section 6). Polygons in px / 30; `"Circle"`: radius `size/30/2`, localPosition (r, r).
  Defaults: density 1, friction 1, restitution 0.2. Vertex limit 20.
- `lastCheckNum` and some level flags (`Level_7.redCheckLevel`, `Level_13.greenCheckLevel`,
  `Level_16.isStrelka`) are **static**: kept by a checkpoint restart (`R`/F key,
  `SetLevel(id, true)`), cleared by a fresh level load.

### 4.2 Per frame (`Game.UpdateHandler` -> `Level.Update` -> `Level_N.Update`)

1. Camera: `dp = Level.x`; broadcast "TweenEvent" (Tweener step, unless `isGless`); `dp -= Level.x`.
2. `m_world.Step(1/30, 10)`.
3. Sprite sync for non-static bodies with a sprite: x/y truncated to twips; `rotation` written as
   `angle*180/PI % 360` (the getter normalises to (-180, 180]).
4. Ground probes; input forces (L/R change vx by 0.5 grounded / 0.25 airborne while |vx| < 5 /
   2.5; Up: ApplyForce (0,-65) when grounded with contacts, else (0,-1) while vy < 0).
   The one-frame key-press flags (`aVariable`/`dVariable`/`wVariable`) are cleared after the
   forces; the held-key flags (`Left`/`Right`/`Up`) are cleared only by `OutControl`.
5. Camera tween target: `-ball.x + 275`, `-ball.y + 200` (31 frames, easeOutExpo, frame-based).
6. Goal `hitTestObject` (no `IsLive` guard) -> `PlayerWin`; spikes (only if alive) -> `PlayerDie`;
   checkpoint `hitTestObject` loop (no `IsLive` guard).
7. If `isGless`: Tweener step here instead.
8. `Level_N.Update`: kill lines, kill bodies, moving parts, switches.

`PlayerWin` and `PlayerDie` both call `OutControl()` (clears key flags), which is why the logger
records input 0 on win and death frames. `Game.frameCount` stops on the win and keeps counting
across `R` restarts.

**Input string (RLE):** `n=0 d=1 w=2 e=3 a=4 S=5 q=6 W=7` (code = 4·L + 2·U + R), each followed
by a count; `R` = checkpoint restart (no tick). TAS strings end at the flag.

### 4.3 Engine quirks replicated (differ from stock Box2D 2.0 C++)

1. Pair buffer not sorted in `Commit` (contacts in insertion order). 2. Separated-point velocity
bias `-60·separation`. 3. Friction clamp uses the pre-update normal impulse. 4. Restitution bias
from pre-warm-start velocities. 5. `Number.MIN_VALUE` where C++ used `FLT_EPSILON`.
6. Uninitialised `Number` fields are NaN. 7. Bodies created static; the type flip in
`SetMassFromShapes` recreates proxies. 8. Listener `splice(body, 1)` always removes index 0; fires
per manifold point and on feature-key changes. 9. Destroyed bodies stay readable. 10. Joints use
the 2.0.x force formulation; contacts solved before joints. 11. `CreateJoint`/`DestroyJoint`
refilter the body with fewer shapes. 12. Prismatic equal-limits position solve uses
`max(linearError, |angularC|)`. 13. `SetLinearVelocity`/`SetAngularVelocity` do not wake bodies.

### 4.4 Decompiler warning

JPEXS silently dropped statements in `b2Collision.FindMaxSeparation`; the AVM2 p-code is
authoritative. Cross-check field writes for any newly ported engine code. Never recompile
`Box2D.*` classes when modding the SWF.

## 5. Speedrun mechanics (from the user's teammate; encoded in the sim)

- **Death warp:** from the frame after a death the ball is off the display list, so its
  flag/checkpoint hit box is compared in its own frozen coordinates while the targets are shifted
  by the camera (which keeps easing for ~1 s). A checkpoint hit this way can be respawned at with
  `R` on the next frame; a flag hit this way wins.
- **Real-game timers** (not in the TAS hack): respawn 1.2 s after death; next level 2.839 s after
  the flag. Both run while paused; their actions only happen unpaused; the camera does not move
  while paused. A death-warp finish needs a pause before the respawn: **flag frame <= death + 38**;
  optimal unpause = **flag + 88** (`DeathWarpFinishValid`, `RESPAWN_LAST_PAUSE_FRAMES`,
  `WIN_TIMER_FRAMES`). TAS hack strings stop at the flag; there is no pause in the TAS hack.
- **Spike glitch** = `isGless` off: the camera steps before physics, so the standardized spike test
  is shifted by `dp`. In the real game a pause < 1 s enables it, > 1 s disables it. TASes assume it
  on. Team test cases (Level 3) are self-tests.
- Team TASes reproduced: Level 4 any% (death 273, flag 274), Level 4 delayed warp (death 285, flag
  309), Level 8 double warp (checkpoint 200, flag 1084), Level 8 TAS (flag 405).

## 6. Calibration facts (details and numbers in `docs/STATS_LOGGING.md` §3.5-3.10)

- `x`/`y` setters truncate toward zero to twips. `sin`/`cos` = Intel LIBM (FP 11.4).
- **Display matrix:** built from the value *written* to `rotation`: `x = trunc(w*65536)` reduced
  into [-180, 180] degrees in integers, then a 0.25-degree sine table (sin·2^30, truncated) with a
  quarter-weight lerp (low 14 bits / 65536), `cos(x) = sin(90 - |x|)`, rounded to 16.16. Exact on
  26,716 matrices (`FlashRotationMatrix`, `Sim::spriteRotW`).
- **Bounding boxes:** rotated half-extent = round-nearest(half · (|a| + |c|)); nested levels are
  rounded one at a time. `hitTestObject` counts touching edges. Known gap: nested rotated+scaled
  sprites (two walls 1 twip off, never hit-tested).
- **`rotation` getter of timeline-placed clips:** not atan2 of the stored matrix (an internal
  approximation). Measured values go in `kTimelineRotations`; the sim aborts on a rotated body
  clip without one. Dynamic bodies: recover from a log's tick-0 angle (unique double). Static
  bodies: need an E9a dump.
- **Camera:** easeOutExpo at t = 1 of 31 frames, `c*1.001*(1 - 2^(-10/31)) + b`, truncated to
  twips; the `pow` constant is stored (`FlashTweenConstant`, 0x3fe996a2ea68dd55).
- **Standardized spikes:** `localToGlobal` truncates the point to twips, applies the matrix,
  rounds to nearest; `cover.x = dp` is truncated; shifted point truncated; strict triangle test.
  Exact (E8c 30,000/30,000). Only translated and quarter-turned spikes are calibrated.
- `TextField` x/y include a 2 px text margin (irrelevant: never bodies).

## 7. Search tools

**`rbsim optimize --level N --inputs RLE`** (src/search.cpp): random local edits of a known route,
replay from cached snapshots, score = flag frame then flag overlap depth, death-warp rule,
`--threads`, `--evals N` (reproducible). Recovers deliberately slowed routes; no gains on the
team's TASes in short runs.

**`rbsim beam --level N [--checkpoint C] [--prefix RLE] --memory 10G`** (src/beam.cpp):
- Layer = frame; each state x 8 inputs; children that win are solutions; dying children are
  played through the death-warp window immediately; exact duplicates merged (hash of the compact
  encoding).
- Score = navigation distance to the flag (Dijkstra on a 10 px grid of static geometry, built from
  the start state) at the position `--lookahead` frames ahead (default 6). Moving bodies ignored.
- Selection `--select mixed` (default: half by score with per-bucket `--diversity`, half coverage
  first), `score`, `coverage`. `--seed-route RLE` keeps a known route in the beam. `--explain RLE`
  prints a route's scores.
- Width from `--memory` (default 4 GB) or `--width`; links per frame in `DIR/links`, beam saved to
  `DIR/beam.bin` every `--save-every` minutes, `--resume`. Route rebuilt and replay-checked.
- **Deterministic** across thread counts, kill -9 + resume, and Windows vs Linux (candidates are
  ordered by score, parent, input; the hash covers struct padding so it must never be used for
  ordering; `sqrt` not `hypot`).
- Results (one core, small widths): Level 2 unseeded 191-215 (TAS 186), seeded 186; Level 4
  mixed finds a death warp by itself (flag 353; TAS 274), `score` alone dies out at the crushers.
- **Main limitation:** the score. On Level 2 a 20,000-wide beam was no better than 2,000-wide;
  `--explain` showed the TAS line is behind the beam's best mid-level and overtakes late.

Measured: compact state ~1.4 KB (L2) to ~9 KB avg / 17 KB worst (L8); encode ~10 us, decode ~5 us;
sim speed per thread ~450k frames/s (L2), ~140k (L12 idle), ~36-60k (L8 with the car). Exhaustive
search grows ~2x per frame even with merging (prototype: last 16 frames of L8 ~207k expansions,
11 s): proofs are only feasible for short windows/endgames.

## 8. Next steps (in the user's order of interest)

1. **Better beam score** (biggest lever): account for moving platforms/timing (e.g. time-indexed
   platform positions in the nav field, or score = elapsed + estimated remaining time); calibrate
   any change with `--explain` against the team's TASes (L2 186, L4 274, L8 405).
2. **Exhaustive window proofs** (resumable, splittable across machines) and **endgame proofs**
   (prove the fastest finish from a state within ~16-20 frames).
3. **Remaining levels:** 13 and 16 (one rotated dynamic body each: tick-0 angle from a log), 17 (a
   few rotated/scaled spike rows: calibrate first), then 9, 10, 11, 15 (many transformed spike
   rows; 10 and 11 have rotated static bodies: need an E9a dump; the mod must run E9a on every
   level, §3.0).
4. Physics profiling for contact-heavy levels (must keep bit-exactness).

## 9. Environment setup (fresh sandbox)

```bash
git clone https://github.com/Theme25/project-unnamed.git && cd project-unnamed
make && ./rbsim test                  # expect ALL PASSED (55)

# Windows cross build + Wine (to check the Windows build)
mv /etc/apt/sources.list.d/nodesource* /tmp/ 2>/dev/null   # a broken repo blocks apt-get update
apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq mingw-w64 wine64
make windows && WINEDEBUG=-all wine rbsim.exe test

# JPEXS FFDec (decompiler); Java is preinstalled
mkdir -p ~/tools/ffdec && cd ~/tools/ffdec
curl -sL -o ffdec.zip https://github.com/jindrapetrik/jpexs-decompiler/releases/download/version15.1.1/ffdec_15.1.1.zip && unzip -q ffdec.zip
java -jar ffdec.jar -cli -export script ~/extracted/practice /mnt/user-data/uploads/Red_Ball_-_Practice_Hack.swf
java -jar ffdec.jar -cli -swf2xml /mnt/user-data/uploads/Red_Ball_-_Practice_Hack.swf ~/extracted/practice_full.xml
# p-code of one class (authoritative):
java -jar ffdec.jar -cli -format script:pcode -selectclass Box2D.Collision.b2Collision \
     -export script ~/extracted/pcode /mnt/user-data/uploads/Red_Ball_-_Practice_Hack.swf

# regenerate data from the SWF XML
make levels SWFXML=~/extracted/practice_full.xml
python3 tools/gen_display_data.py ~/extracted/practice_full.xml > src/display_data.h
```

Symbol ids: `Levels.Level_1` 401, `_2` 355, `_3` 575, `_4` 561, `_5` 125, `_6` 434, `_7` 287,
`_8` 389, `_9` 248, `_10` 205, `_11` 87, `_12` 164, `_13` 601, `_14` 536, `_15` 474, `_16` 328,
`_17` 493; `PlayerBox` 23; `Shipik` 57; `Ships10` 58.

**Pitfalls learned the hard way:**
- **Never change the build flags.** `-O3 -march=native` broke bit-exactness on Levels 3, 8, 12 and
  was not faster. Keep `-O2 -ffp-contract=off -fno-fast-math -fexcess-precision=standard`.
- Avoid host libm functions in anything that affects results (`pow`, `hypot`, `atan2`, ...): they
  can differ between Linux and Windows. `sqrt`, `fmod`, `floor`, `trunc` are exact.
- Tool calls time out at 300 s: run long jobs detached
  (`setsid nohup sh -c '... > log 2>&1' &`) and poll the log. Kill with `pkill -x rbsim`
  (`pkill -f` can match and kill your own shell).
- Two runs writing the same beam `--dir`/log corrupt each other.

## 10. Verification and delivery workflow

**Verifying a level:** the user records logs per `docs/STATS_LOGGING.md` §3.0/§6 (idle, each death
kind, checkpoint-1 restart, win/TAS, level-specific cases) and a calibration dump (`E9level`,
E9a, E10). Run `./rbsim verify <log>` (look for `diverged: 0`) and `./rbsim calib <dump>`
(`CALIB OK`). Before every commit: `./rbsim test` + verify every log available in the chat.

Known log artifacts handled by `verify`: death and win rows log input 0 (retried with the
previous input); rows logged after the frame counter stops are skipped (but the first `ts=1` win
row is compared); frame counter compared segment-relative; static flags carried after `R`.
Earlier wrong turns worth remembering: an ActiveX-host log looked like a physics bug; logs
recorded with `MATHSPIKES 0` looked like spike bugs (check the mod settings first).

**Delivering** (the assistant cannot push):
1. Commit with the user's identity:
   `git -c user.name="Mohamad Shaikh Khalil" -c user.email="Mohamad.sk.work@gmail.com" commit ...`
2. `git fetch origin` and bundle everything the remote lacks:
   `git bundle create /mnt/user-data/outputs/rbsim-update.bundle origin/main..HEAD`
3. For Windows users also ship `rbsim.exe` (`make windows`) and `docs/WINDOWS.md`.

The user runs in an up-to-date clone: `git pull rbsim-update.bundle main` then `git push`.
"Repository lacks these prerequisite commits" means his clone is behind: `git fetch origin &&
git reset --hard origin/main`, then pull again. (Last chat's bundles were made from `a1aafab..HEAD`;
check `origin/main` first, he may have pushed some or all of them.)

## 11. Licensing

`src/libm_intel.S` derives from OpenJDK HotSpot (Intel copyright, GPL-2.0-only), so binaries
that include it are GPLv2. The rest is the user's own code; no license chosen yet.
