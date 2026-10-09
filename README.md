# rbsim — bit-exact Red Ball 1 simulator

A C++17 port of the Box2DFlash 2.0.x engine embedded in *Red Ball 1*
(practice-hack / tournament SWFs; their Box2D code is byte-identical), plus the game-side
logic (level scripts, display-layer hit tests, camera, standardized spikes, death and death
warps), built as the foundation for brute-force route search. Reference player: **Flash
Player 11.4.402.287** with the Practice Hack, `MATHSPIKES = 1`, spike glitch on (`isGless` off).

## Status at a glance

| | |
|---|---|
| levels bit-exact against Flash logs | **all 17** (81 logs, 97,966 frames) |
| open details | turned/scaled spike rows and the turned-flag box rounding are modelled, not calibrated; every logged decision matched (see Level status) |
| search | `rbsim beam` (route finding from any start, resumable), `rbsim optimize` (improves a known route) |

## Build & run

**Linux / WSL / Codespaces:** `make`. **Windows:** see "Windows" below, or [`docs/WINDOWS.md`](docs/WINDOWS.md) to just run the exe.

```
make                 # see "Build flags" below; do not change them
./rbsim test         # 85 self-tests (physics, display layer, spikes, death warps, snapshots, ...)
./rbsim run --level 2 --inputs "n20w1d25n40" [--hex] [--every N] [--checkpoint K]
./rbsim log --level 2 --inputs "..."      # simulator log in the Flash mod's TSV format
./rbsim bench        # ~0.8-0.9M frames/s/thread on Level 1 incl. full-state restores
./rbsim verify rb1_stats.tsv [--verbose N] [--ignore sr,...] [--trig intel|glibc]
./rbsim calib rb1_calib.tsv                # checks a calibration dump (docs/STATS_LOGGING.md)
./rbsim optimize --level N --inputs RLE    # improve a known route, see below
./rbsim beam --level N --memory 10G        # find a route from any start, see below
```

`run` prints position, velocity, angle, angular velocity, ground probes (L/C/R), contact count,
alive and sleep flags per frame (`--hex`: raw IEEE-754 bits). Inputs use the game's RLE format
(`n d w e a S q W`, `R` = checkpoint restart); the legacy digit format is also accepted.

### Windows

**Just want to run it?** See [`docs/WINDOWS.md`](docs/WINDOWS.md): using the ready-made
`rbsim.exe`, the common commands, PowerShell vs Command Prompt, and troubleshooting.

Two ways to build, both giving a static `rbsim.exe` (no DLLs needed) with the same results as Linux:

1. **Native, with MSYS2:** install [MSYS2](https://www.msys2.org/), open the **MSYS2 UCRT64**
   shell, then
   ```
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc make git
   git clone <repo> && cd project-unnamed
   make            # produces rbsim.exe
   ./rbsim.exe test
   ```
   Run it from that shell or from `cmd`/PowerShell (`rbsim.exe optimize ...`).
2. **Cross-compiled on Linux:** `apt install mingw-w64`, then `make windows`.

MSVC is not supported: Flash's `sin`/`cos` (`src/libm_intel.S`) is GNU assembly. On Windows the
same routine bodies are used, wrapped so they preserve the registers the Windows x64 calling
convention requires (`rsi`, `rdi`, `xmm6`, `xmm7`). The simulator does not depend on the host's
math library: the only libm functions it uses are exact ones (`sqrt`, `fmod`, `floor`, `trunc`);
the camera's `pow` result is a stored constant.

Checked (cross-compiled build under Wine 9): all 85 self-tests, all 10 calibration files, all
48 stats logs (59,741 frames, 0 divergences), and `rbsim optimize --threads 1 --seed S --evals N`
gives byte-identical output to the Linux build. Running `rbsim.exe test` once on a real Windows
PC is still a good idea.

**Build flags:** use the Makefile's (`-O2 -ffp-contract=off -fno-fast-math
-fexcess-precision=standard`, `-pthread`). `-O3 -march=native` was tried: it breaks
bit-exactness on Levels 3, 8 and 12 and is not faster. Never add fast-math, FMA contraction or
`-march` flags.

## Searching for faster routes: `rbsim optimize`

```
rbsim optimize --level N --inputs RLE [--checkpoint C] [--time SEC] [--threads N]
               [--seed S] [--sideways P] [--evals N] [--gless] [--quiet]
```

Starts from an existing route (one attempt, no `R`) and searches for edits that collect the flag
earlier. Each thread repeatedly applies a random edit to the current best route (flip a frame,
move a run boundary by 1-3 frames, delete or insert a frame, overwrite a 2-8 frame block, or two
of these), replays it from a cached snapshot taken just before the first changed frame, and
stops as soon as it can no longer match the best.

- **Score:** the flag frame; ties are broken by how deep the ball is inside the flag's box on that
  frame (twips; deeper = closer to the next frame). Equal routes are also accepted sideways
  with probability `--sideways` (default 0.05) to cross plateaus.
- **Death warps** count only if usable in a real run (flag no later than death + 38 frames);
  the output gives the pause/unpause frames.
- **Output:** each improvement as it is found, then the best route as an RLE string. If any hit
  test on the route was within one matrix unit of flipping, it says so: check that route in Flash.
- Defaults: mathspikes 1, spike glitch on (`--gless` for glitchless), all hardware threads,
  30 s. Results with several threads are not reproducible run to run (threads race). With
  `--threads 1 --seed S --evals N` (stop after N candidates instead of a time limit) the result
  is identical on any machine and OS.
- It is a local search: it improves a known route but proves nothing. Exhaustive methods
  (beam search, window proofs, endgame proofs) are the next step (see Roadmap).

Throughput depends on the level: one thread simulates about 450,000 frames/s on Level 2 and
about 60,000 on Level 8 (the car adds up to 53 contacts). Tested by slowing known routes:
Level 8 424 -> 405 frames in 60 s, Level 2 200 -> 189 in 30 s (one thread). The team's
Level 2, 4 and 8 TASes are not improved by short runs.

## Finding routes from any start: `rbsim beam`

```
rbsim beam --level N [--checkpoint C] [--prefix RLE] [--memory 10G | --width W] [--threads N]
           [--max-frames F] [--diversity K] [--lookahead T] [--select mixed|score|coverage]
           [--seed-route RLE] [--explain RLE] [--dir DIR] [--save-every MIN] [--resume] [--gless]
```

A breadth-first beam search: starting from the level start, a checkpoint, or the state after
`--prefix` inputs, it keeps up to W states after every frame, expanding each with all 8 inputs.
It does not need a known route.

- **Score:** distance to the flag along the level's static geometry (a navigation grid built once
  from the start state), measured from where the ball would be `--lookahead` frames ahead at its
  current velocity (default 6). Moving bodies are not part of the grid.
- **Per frame:** children that collect the flag are solutions; children that die are played
  through the death-warp window at once and kept only if they warp to the flag in time for a
  real run. Exact duplicate states are merged.
- **Selection** (`--select`): `score` keeps the best scores (at most `--diversity`, default 16, per
  region/velocity bucket); `coverage` keeps the best state of every bucket first, so lines that
  are behind now but ahead later (waiting for a crusher, timing a platform) survive; `mixed`
  (default) fills half the beam each way.
- **`--seed-route RLE`:** a known route whose states are kept in the beam every frame, so the
  result can only match or beat it while the beam explores around and away from it.
- **`--explain RLE`:** prints the score a known route gets every 10 frames (no search); compare it
  with the beam's progress lines to see where a beam would have dropped that route.
- **Width:** `--memory` (default 4 GB) sets W from the measured state sizes, re-checked every
  frame; `--width` fixes it instead.
- **Stops** when no state can reach the flag before the best solution found, at `--max-frames`
  (default 3000), or if the beam empties. The route is rebuilt from per-frame link files and
  re-checked by a replay from scratch before it is printed.
- **Long runs:** the beam is saved to `DIR/beam.bin` every `--save-every` minutes (default 10)
  and at the end; `--resume` (same `--level/--checkpoint/--prefix/--gless`) continues an
  interrupted run. `DIR` defaults to `beam_L<level>`.
- **Deterministic:** the result does not depend on the thread count, on interruptions (a run
  killed with `kill -9` and resumed prints the same route), or on the OS (the Windows build
  prints the same route). Checked on Level 2.

Results so far (one core, small widths; the TASes are 186 on Level 2 and 274 on Level 4):

| run | result |
|---|---|
| Level 2, width 400-20,000 | flag on frame 191-215 (varies with width and tie-breaking) |
| Level 2, width 2,000, `--seed-route` TAS | frame 186 (TAS matched, deeper flag overlap) |
| Level 4, width 2,000, `score` | beam dies out at frame 126 (all states walk into the crushers) |
| Level 4, width 2,000, `coverage` | **finds a death warp by itself**: flag on frame 371 |
| Level 4, width 2,000, `mixed` | death warp, flag on frame 353 |

A beam search finds strong routes but proves nothing, and its quality depends mostly on the
score: on Level 2 a 20,000-wide beam did no better than a 2,000-wide one. `--explain` showed
why: the TAS line is *behind* the beam's best states mid-level and only overtakes them near the
end.

Memory per state (average along real routes): Level 2 ~1.4 KB, Level 4 ~3.3 KB, Level 12
~6 KB, Level 8 ~9 KB. A 1M-wide beam is ~3 GB on Level 2 and ~20 GB on Level 8.

**Compact snapshots** (`src/snapshot.h`): a state is stored as the 8-byte blocks that differ from
the freshly loaded level (average ~6 KB, worst ~17 KB, vs 152 KB for a full `Sim`). Restoring
copies the base and patches the blocks back, so it is byte-identical by construction; encodings
are portable to another process (same binary and level), which makes saved/resumable searches
possible. Encode ~10 us, decode ~5 us.

## Architecture

| file | contents |
|---|---|
| `src/b2math.h` | AS3-exact vector/matrix/sweep math; `Number.MIN_VALUE` epsilons; the single `as3_sin/as3_cos` choke point |
| `src/libm_intel.S` | Flash Player 11.4's `sin`/`cos` (Intel LIBM, generated by `tools/hotspot2gas.py`) |
| `src/b2collision.cpp` | polygon geometry, circle/poly narrow phase, GJK distance, TOI |
| `src/b2world.cpp` / `b2world.h` | SAP broadphase, pair manager, contact lifecycle and listener emulation, island + contact solver, TOI loop, bodies/shapes, filtering |
| `src/b2joints.cpp` | distance, prismatic (limits, motors) and revolute (motors) joints, `CreateJoint`/`DestroyJoint` |
| `src/redball.cpp` / `redball.h` | base `Level` logic (construction, sprite sync, probes, input, camera tween, display layer, spikes, death and post-death update), level scripts 1-17, RLE codec |
| `src/levels_data.h` | named placements of all 17 levels (`tools/extract_levels.py`, `tools/gen_levels_data.py`) |
| `src/level_polys.h` | polygon tables of levels 9-17 as written in the AS3 (`tools/gen_level_polys.py` from the JPEXS export) |
| `src/display_data.h` | bounds of every named object and every spike, per level (`tools/gen_display_data.py`, `tools/swf_geom.py`) |
| `src/flash_sintab.h` | Flash's display-matrix sine table (`tools/gen_flash_sintab.py`) |
| `src/snapshot.cpp` / `snapshot.h` | compact, portable, byte-exact snapshots |
| `src/search.cpp` | `rbsim optimize` |
| `src/beam.cpp` | `rbsim beam` (navigation field, beam, links, save/resume) |
| `src/verify.cpp` | replays stats logs and compares every field bit-for-bit |
| `src/calib.cpp` | checks calibration dumps (trig, bounds, matrices, spikes, placements) |
| `src/rbsim.cpp` | CLI, self-tests, benchmark |
| `tools/trig_flip_search.cpp` | diagnoses divergences caused by 1-ulp `sin`/`cos` differences |

- **Snapshots:** all mutable state lives in fixed arrays linked by `int` indices, so
  `Sim copy = sim;` is a complete, bit-exact snapshot (152 KB); `snapshot.h` stores it compactly.
- **Shared geometry:** immutable polygon data lives in a `GeomTable` that snapshots share.
- **Scratch memory:** island and constraint buffers are thread-local, so one sim per thread is safe.

## What is modelled beyond the physics

All of these are verified against Flash logs and calibration dumps (details and numbers in
`docs/STATS_LOGGING.md` §3.5-3.10).

- **Display matrix:** Flash builds a sprite's 16.16 matrix from the value *written* to
  `rotation` (truncated to 16.16 fixed-point degrees, reduced mod 360 in integers) with its own
  0.25-degree sine table and a quarter-weight interpolation bug; exact on 26,716 matrices.
- **Hit tests:** `hitTestObject` on twip bounding boxes (touching edges count); rotated boxes
  round each half-extent to the nearest twip, one nesting level at a time. A **turned multi-part
  clip** (Level 11's pushed flag: pole + cloth) is boxed per part, each part's box transformed and
  then unioned; rotating the union box instead got 10 of 11 near-contact frames wrong. (`getBounds`
  of a nested sprite, E10, follows the union rule: Level 11 `triangle`. So `hitTestObject` and
  `getBounds` are not the same computation for turned clips.)
- **Camera:** Tweener easeOutExpo at t = 1 of 31 frames per `Update`, truncated to twips; `dp` is
  the camera step of the frame.
- **Standardized spikes** (`MATHSPIKES = 1`): 16 control points, `getBounds` test, shift by `-dp`,
  strict triangle test, with Flash's twip truncation/rounding (30,000/30,000 calibration cases).
  This shift is the **spike glitch**; `--gless` gives glitchless mode.
- **Death:** `PlayerDie` destroys the ball's body and the level keeps running its full `Update`
  (world step, moving parts, `Level_N.Update`); the camera follows the frozen ball and the goal,
  checkpoint and switch tests (no `IsLive()` guard) compare the ball, now off the display list,
  with targets shifted by the camera: the **death warp**, also onto a moving flag (Level 11).
  Real-game timers are checked as a route rule (flag no later than death + 38 frames; unpause at
  flag + 88). After deaths `verify` also reports how many frames the level's bodies stayed
  bit-exact; they differ only where the random debris hits something (so far always the body that
  killed the ball).
- **Static level flags** (`Level_7.redCheckLevel`) survive checkpoint restarts.
- **Not simulated:** death debris (8 bodies at `Math.random` positions, so a route that relies on
  them is not reproducible in the real game either); pausing (the TAS hack has no pause).

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

## Verification status

Every field of every frame is compared bit-for-bit against stats logs recorded with the mod
described in `docs/STATS_LOGGING.md` (Flash Player 11.4.402.287, Windows XP, x86). All logs
below match on every frame compared.

| level | logs | frames compared | deaths on the Flash tick | notes |
|---|---|---|---|---|
| 1 | 2 | 1,204 | — | incl. the Level 1 TAS (win at tick 510); earlier sessions |
| 2 | 8 | 23,729 | 1 | 18,595 of them after a death (camera, `dp`, frame counter) |
| 3 | 5 | 2,329 | 10 | spike deaths, 2 wins, checkpoint restart |
| 4 | 2 | 3,365 | 2 | death-warp TASes: any% (flag 274), delayed warp (flag 309) |
| 5 | 4 | 4,626 | 1 | blue switch, 2 wins |
| 6 | 4 | 3,320 | 1 | drop platforms, checkpoint restart, win, spike death |
| 7 | 5 | 4,426 | 2 | red switch carried across R, blue switch, 2 wins |
| 8 | 12 | 10,009 | 7 | car, crushers, ramps, wall spikes, double death warp (checkpoint 200, flag 1084), 5 wins |
| 9 | 7 | 7,775 | 7 | idle (spike death at 130), spike, kill roll ball and boom-crank deaths, green switch (3 logs), crank riding, checkpoint-3 restart and win (224), death-warp win (death 358 on `boomCrank`, flag 359); 4,676 frames after a death |
| 10 | 4 | 6,241 | 6 | idle, fall deaths (incl. from checkpoint 1 after R), `roundBlock` death (171), jump platforms 1-2, `cube3`, `goBall`, TAS win (394) |
| 11 | 5 | 4,507 | 3 | idle train ride, `killRotate` death (334), spike death on a turned row (564), the flag pushed and turned by the train (-4 to 49 degrees), a fall death followed by a death warp onto the still-moving flag (death 1110, flag 1117), win (309) |
| 12 | 5 | 5,006 | 2 | collision groups, checkpoint-2 restart, 2 wins |
| 13 | 4 | 4,022 | 5 | idle (the loose `kingStar1` rolls into the ball at 145), `kingStar1` and `killStar2` deaths, moving platforms, R from checkpoints 1 and 2, 2 wins |
| 14 | 3 | 2,931 | 1 | switch, drop platform, checkpoint restart, win |
| 15 | 4 | 3,573 | 4 | idle, `killLine` deaths after R from checkpoint 2 (built without `luk`), red switch (gate rebuilt as a hinged door spinning at 3 rad/s), skateboard, 2 wins (204, 464) |
| 16 | 5 | 4,414 | 3 | idle, `killRollBall1` death that also collects checkpoint 1, drop platform, `wrongWay` resetting `lastCheckNum` (alive, and as a death warp), `isStrelka` kept across R, a spike death, 2 wins; the same route recorded with `isGless` off (survives the spike at 322: spike glitch) and on (dies at 322) |
| 17 | 4 | 7,693 | 9 | idle fall death (78, rolls off the crown), `killStarPart` death, spike deaths on the 15-degree row (5, after R from checkpoint 1), spinning star, moving platform 1, jump platform, TAS win (338) |

The ball's display matrix, the camera, `dp`, `lastCheckNum` and the level flags are also compared
on every frame where the log has them. Logger artifacts handled by `verify`: death and win rows
log input 0 (`OutControl` clears the key flags before logging) and are retried with the previous
input; rows logged after the frame counter stops are skipped; `Game.frameCount` continues
across `R` restarts.

An older custom ActiveX host (Flash 11.5.502.149, Windows 8) also ran 388 Level 1 logs
(113,938 frames): 376 exact, 12 diverge because that host's `sin`/`cos` differ:

- **Flash Player 11.4 uses Intel's LIBM SSE2 routines.** `src/libm_intel.S` is a mechanical
  translation of those routines from OpenJDK (`tools/hotspot2gas.py`). It matches 4,096/4,096
  Flash calibration samples and 83,360/83,360 values of Java's `Math.sin`/`cos`, and is the
  default (`--trig intel`).
- **The ActiveX host differs from Intel LIBM, glibc, x87 `fsin`/`fcos` and fdlibm** by 1 ulp on
  about 3.5% of inputs; its libm is unidentified. Use Flash Player 11.4 for all logs.

Calibration results:

| question | answer |
|---|---|
| twip quantisation of `x`/`y` | truncation toward zero (2,400/2,400) |
| `rotation` getter | written value, `fmod`-normalised to (-180, 180] (720/720); for timeline-placed clips Flash reports its own value (measured per level, `kTimelineRotations`) |
| display matrix from `rotation` | see "What is modelled" (26,716/26,716) |
| x87 extended precision in physics | no; all arithmetic matches SSE2 doubles |
| `Math.sin`/`cos` (FP 11.4) | Intel LIBM (0/4,096 mismatches) |
| `Math.atan2` (FP 11.4) | x87 `fpatan` (0/4,096); not used by the physics |
| `localToGlobal` / spike `testPoint` | twip truncation and rounding as in Flash (16,000 + 26,040 cases) |

**Licensing note:** `src/libm_intel.S` is derived from OpenJDK HotSpot code,
which is GPL-2.0-only (Intel copyright). Binaries that include it fall under
GPLv2.

**Platform note:** x86-64 only. Linux and Windows (MinGW-w64) are supported and checked;
macOS (x86-64) would need the assembly's section/symbol directives adapted; ARM machines
(including Apple Silicon) cannot run the Intel `sin`/`cos` routine.

## Level status

| level | status |
|---|---|
| 1–17 | bit-exact (see Verification status) |
| 5 | green switch not yet exercised by a log |
| 9 | not yet exercised by a log: the fall death (y > 530), checkpoints 1 and 2 as restart points. Its rotated spike rows (2.48 and 25.28 degrees) use a continuous inverse transform that is not calibrated; on the logs every spike decision matched and none was within a rounding unit of flipping (`verify` reports this count) |
| 10 | not touched by any log: `afterJump` (rotated static body; its rotation is Flash's E9a value), `cube1`, `cube2`, `jumpPlatform3`, checkpoints 2 and 3. They use the same calibrated data and mechanics as the logged parts. No spike death: the 205 plain spike rows use the calibrated model (Levels 3, 6, 8); the 13 rotated/scaled spikes at the two ends of the pit before the flag (x 1015-1050 and 1820-1850) are uncalibrated, and the TAS passes 15 px from one. `calib`: three walls 1 twip off (nested scaled sprites, never hit-tested) |
| 11 | not exercised by a log: deaths on `killStar0-2`, `killBrevno0-3`, `killCeil1` (same contact-list kill test as `killRotate`). The turned flag's box: the corner rounding (nearest/floor/truncation) is not determined by the logs; `verify`/`displayUncertain` count decisions that depend on it (none so far) |
| 13 | not exercised by a log: the green switch (removes `greenBarier`; `Level_13.greenCheckLevel` is static and survives R, same code path as Level 7's verified red switch), the fall death, a `killStar1` death (same contact kill test as `killStar2`) |
| 15 | not exercised by a log: the fall death (y > 860); a spike death. All 44 spike rows are turned and scaled (about 0.71 x 0.6), uncalibrated; both winning runs pass through their bounds (70 frames) with every decision matching Flash and none within a rounding unit of flipping |
| 16 | not exercised by a log: the blue switches (three switches destroy the same plate; same mechanism as Levels 5 and 14), deaths on `axe1` and `killRollBall2`. `wrongWay` is a turned target (5.5 degrees, shape + text field); both logged hits matched on the frame, its edge rounding is not calibrated (`displayUncertain` counts near misses: none) |
| 17 | not exercised by a log: `movePlatform2` (its direction starts at 0: AS3 quirk, reproduced), `kingCrown2`'s far side |

Levels 9-17 are ported from the SWF's `Level_N.as` (polygon tables generated by `tools/gen_level_polys.py`).

**Turned and scaled spikes** (Levels 9, 10, 11, 15, 17) use a continuous inverse transform plus Flash's twip
conversions, not a calibrated table. Evidence: spike deaths on turned rows matched Flash on the exact frame (Level 9
2.48 degrees, Level 11 -3.58 degrees, Level 17 15 degrees five times), Level 15's winning runs cross its scaled rows
for 70 frames with the same no-hit decisions, and no logged decision was within one rounding unit of flipping
(`verify` reports this count; a route that depends on such a decision should be checked in Flash).

Quirks of the AS3 that are reproduced: Level 17 `movePlatform2BodyDirection` is never initialised (0) and `movePlatform1BodyDirection` is set twice (ending -1); Level 16 `wrongWay` resets `lastCheckNum` to 0 (also after death); Level 15 `luk` exists only when `lastCheckNum == 0`; repeated `DestroyBody` of the same body (Levels 9, 13, 16) is a no-op. (Level 9 and 16 direction fields have declaration initialisers: Level 9 both 1; Level 16 roll balls 1 and -1. An earlier note said they were 0; the Level 9 logs showed otherwise.)

Known open detail: Flash's rounding of nested rotated+scaled sprite bounds is not modelled
(1 twip off on two walls that are never hit-tested); a rotated/scaled *target* in a later level
would need it.

Joint code: every field write in all 48 joint methods was cross-checked against the AVM2 p-code
(no decompiler losses). Only distance, prismatic and revolute joints are ever created by the game.

## Roadmap

1. Search: a better beam score (account for moving platforms and timing; calibrate against known
   TASes with `--explain`); resumable, splittable window proofs; endgame proofs; physics profiling
   for contact-heavy levels.
2. Simulator: calibration sweeps for turned/scaled spikes (E11) and the turned Level 11 flag (E12) are ready
   (`docs/mod_sweeps.as`, checked by `rbsim calib`); they only matter for decisions within a twip of an edge.
