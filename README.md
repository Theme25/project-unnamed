# rbsim — bit-exact Red Ball 1 simulator

A C++17 port of the Box2DFlash 2.0.x engine embedded in *Red Ball 1* (practice-hack / tournament
SWFs; their Box2D code is byte-identical), plus all of the game-side logic: the 17 level scripts,
display-layer hit tests, camera, standardized spikes, death and death warps. It is the foundation
for TAS route work.

**Reference:** Flash Player **11.4.402.287** with the Practice Hack, `MATHSPIKES = 1`, spike glitch
on (`isGless` off; glitchless mode is supported too). "Bit-exact" means every logged value of every
frame matches that Flash Player to the last bit.

## Status

| | |
|---|---|
| levels | **all 17 bit-exact** against Flash logs: 83 logs, 99,170 frames, every death and win on Flash's frame |
| display layer | ported from Flash Player 11.4 itself (decompiled, `docs/FLASH_PLAYER_RE.md`) and checked against every dump: rotation matrices, hit-test boxes, camera, plain spikes (E8c 30,000/30,000), turned/scaled spikes (E11 390,670/390,670), the turned Level 11 flag (E12 148,074/148,074) |
| real-game spikes | the original game's / Tournament Edition's pixel spike test (`--mathspikes 0`) at any window size and scale mode, including the moving-camera spike glitch: 28 real-time Flash playbacks bit-exact to the death frame (`docs/RESOLUTION.md`) |
| death | death warps (onto checkpoints, flags, a moving flag, the Level 16 wrong-way trigger) and real-game pause timing |
| platforms | Linux and Windows (x86-64), identical results; `rbview.exe`: the Practice Hack's TAS mode for Windows (same keys) with savestates, ghost and route viewer |
| smartforce | `rbsim smartforce`: simulates every string of an input pattern and reports those meeting conditions |
| 64-bit players | `--player activex` (ActiveX Flash Player + 11.5 x64 OCX), `--player x64` / `x64-nofma` (Flash Player 30 x64): their own `Math.sin`/`cos` and display arithmetic (`docs/X64_PLAYERS.md`); also in rbview's Options tab |
| tests | `rbsim test`: 99 self-tests; `make tastest`: the TAS core of the viewer |

## What's new in v2.0

- **Red Ball TAS (`rbview.exe`)** is now the Practice Hack's TAS mode on the simulator: the mod's keys and
  behaviour, with a proper UI (below). New since v1.2:
  - savestates without a limit, with names, kept in a file, plus backups of every route a restart, rewind or
    load replaced;
  - a ghost route to race against, and the jump brute-forcer's candidates actually simulated;
  - every setting the mod has as a key or a command is also a checkbox or slider (Options tab);
  - Space can be held to keep stepping while you press other keys;
  - the HUD grows with the window and shows the distance to the flag and the jump probes (also drawn under
    the ball, Shift+J);
  - spike hitboxes are drawn as the game tests them (Shift+H). Under `mathspikes 0` they are the exact window
    pixels that kill at the chosen window size;
  - the view follows the game camera unless you drag it.
- **`rbsim smartforce`** (contributed by @donaldkabelhdmi in PR #2) simulates every input string of a
  hand-written pattern and lists those that meet your conditions (section below). It replaces `rbsim optimize`,
  `rbsim beam` and the viewer's Search tab; those are in the git history up to commit `7a21128`.
- **Faster engine** (PR #2): exact shortcuts, 1.31x on Level 14 replays. The results are unchanged bit for
  bit: per-frame full-state digests match the previous engine on 2,040 random runs over all 17 levels,
  checkpoints, restarts and the three spike rules.
- **Fixes:**
  - Level 15 started from a checkpoint and then from the beginning closed the program ("new body name after
    template freeze"). The loose plank exists only from the level start; every start is now registered when
    a level is first built.
  - Drop platforms re-created more than 96 times (riding one on Level 4, 6 or 14) closed the program ("body
    capacity exceeded"). The slots of destroyed bodies are now reused, only once the slots run out, so
    verified routes are unchanged.
  - Windows: a crash when worker threads ended (the physics scratch buffers now use fiber-local storage).

## Red Ball TAS / route viewer (Windows, no command line)

**[⬇ Download rbview.exe](https://github.com/Theme25/project-unnamed/releases/latest/download/rbview.exe)**
(latest release; one file, no installer). Windows may show "Windows protected your PC" the first time
because the exe is not signed: click **More info**, then **Run anyway**.

`rbview.exe` is the Practice Hack's TAS mode on the bit-exact simulator: **the same keys and behaviour as the
mod** (A/W/D to play, T pause, Space frame advance, `,` `.` speed, 0-9 / Shift+0-9 states, I/O import/export,
Backspace rewind, R/F restarts, G invincibility, Shift+G glitchless, H/J overlays, B/Shift+B jump brute-forcer,
`/config`, `/set` warps, `/remap`), with a proper UI around it:
- savestates without a limit, with names, kept in a file, plus backups of every route a restart, rewind or load
  replaced;
- a ghost route to race against;
- the jump brute-forcer's candidates actually simulated;
- Smartforce searches with **Settings / Advanced settings**, live output, scrollable forms,
  uncapped result saving, a graceful Stop button and a **String syntax** guide;
- remappable keys that stay out of the way while you type, and every setting as a checkbox or slider;
- the route viewer: seek any frame, events list, death-warp ghost, path, a HUD that scales with the window
  (distance to the flag, jump probes), spike hitboxes as the game tests them, Flash's spike test at any window
  size.
Guide and full key list: [`docs/WINDOWS.md`](docs/WINDOWS.md).
Native Windows build of the viewer, including Smartforce: `tools/build-rbview.ps1` with MinGW-w64's
`g++` and `windres` on PATH (or pass `-Cxx` and `-Windres` paths).
Build: `make viewer` (MinGW-w64 cross build; Win32 + GDI+, static; source in `viewer/`: `tas.cpp` is the TAS logic
without Windows code, checked natively by `make tastest && viewer/tas_test`).
To publish a new version: create a GitHub release (new tag, e.g. `v2.0`) and attach the exe named
exactly `rbview.exe`, so the download link above keeps pointing at the newest one
(`gh release create v2.0 rbview.exe rbsim.exe --title "..." --notes "..."` does the same from the command line).

![rbview: Level 2, the TAS against a ghost 8 frames behind, with the HUD, the jump probes and the path](docs/rbview.png)

## Build & run

**Linux / WSL / Codespaces:** `make`. **Windows:** see [Windows](#windows) below, or
[`docs/WINDOWS.md`](docs/WINDOWS.md) to just use the ready-made exe.

```
make                 # see "Build flags" below; do not change them
./rbsim test         # 99 self-tests (physics, display layer, spikes, death warps, all 17 levels, snapshots, ...)
./rbsim run --level 2 --inputs "n20w1d25n40" [--hex] [--every N] [--checkpoint K]
./rbsim log --level 2 --inputs "..."      # simulator log in the Flash mod's TSV format
./rbsim bench [--frames N]                # throughput
./rbsim verify rb1_stats.tsv [--verbose N] [--ignore sr,...] [--gless 0|1] [--trig intel|glibc]
./rbsim calib rb1_calib.tsv                # checks a calibration dump (docs/STATS_LOGGING.md)
./rbsim smartforce --level N --inputs PATTERN --report CONDITIONS   # try many routes at once, see below
# any command: --mathspikes 1|2 picks the spike rule (docs/MATHSPIKES_RESOLUTION.md);
#   --mathspikes 0 --window WxH [--noscale]: the original game / Tournament Edition at that window (docs/RESOLUTION.md)
#   --player 11.4|activex|x64|x64-nofma: whose arithmetic (default 11.4 = every 32-bit player; docs/X64_PLAYERS.md)
make framescan && tools/framescan rb1_stats.tsv      # checks the frame-scan mod's edge scans (docs/RESOLUTION.md)
make deathcause && tools/deathcause <level> <RLE> [checkpoint] [gless]   # what killed the ball, when
```

`run` prints position, velocity, angle, angular velocity, ground probes (L/C/R), contact count,
alive and sleep flags per frame (`--hex`: raw IEEE-754 bits). Inputs use the game's RLE format
(`n d w e a S q W` = none, right, up, up+right, left, left+right, left+up, all three, each followed by
a frame count; `R` = checkpoint restart).

`verify` replays a stats log recorded with the logging mod and compares every field of every frame.
It detects per segment whether the recording had `isGless` on (`--gless` forces it), takes the spike rule, window
size and scale mode from the log's `#ENV` line, and also reports how many hit-test decisions came within a rounding
unit of flipping (should be 0).

### Windows

**Just want to run it?** See [`docs/WINDOWS.md`](docs/WINDOWS.md).

Two ways to build, both giving a static `rbsim.exe` (no DLLs needed) with the same results as Linux:

1. **Native, with MSYS2:** install [MSYS2](https://www.msys2.org/), open the **MSYS2 UCRT64** shell, then
   ```
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc make git
   git clone https://github.com/Theme25/project-unnamed.git && cd project-unnamed
   make            # produces rbsim.exe
   ./rbsim.exe test
   ```
2. **Cross-compiled on Linux:** `apt install mingw-w64`, then `make windows`.

MSVC is not supported: Flash's `sin`/`cos` (`src/libm_intel.S`, and `src/libm_x64_*.S` for the 64-bit players) is GNU assembly, wrapped on Windows
to preserve the registers the Windows x64 calling convention requires. The simulator does not depend
on the host's math library (it only uses exact functions: `sqrt`, `fmod`, `floor`, `trunc`).

Checked under Wine 9 after every change: all self-tests, the calibration dumps and the stats logs give
the same results as Linux.

**Build flags:** use the Makefile's (`-O2 -ffp-contract=off -fno-fast-math
-fexcess-precision=standard`, `-pthread`). `-O3 -march=native` breaks bit-exactness on Levels 3, 8
and 12 and is not faster. Never add fast-math, FMA contraction or `-march` flags.

### Tests

The source checkout includes regression tests that do not require saved search results:

```sh
make test
make tastest
./viewer/tas_test
python3 tools/test-smartforce-endpoints.py --binary ./rbsim --out-dir ./test-output
```

On Windows, use `rbsim.exe` for `--binary` and `viewer/tas_test.exe` for the TAS tests.
The viewer's search worker can also be checked in PowerShell:

```powershell
.\tools\test-rbview-smartforce.ps1 -Viewer .\rbview.exe -Cli .\rbsim.exe -OutDir .\test-output
```

## Trying many routes at once: `rbsim smartforce`

```
rbsim smartforce --level N --inputs PATTERN [--report CONDITIONS] [--checkpoint C] [--gless] [--pad N]
                 [--threads N] [--top N] [--list] [--names] [--idle-up] [--hardfilter [--tolerance T]]
                 [--merge-report] [--maxmem GB] [--stochastic [--time SEC] [--samples N] [--seed S] [--start-percent P]]
                 [--sortby [-]Q] [--exclude FILE] [--save [FILE]]
```

`--save` writes every matching result to a file when the search ends (also after Ctrl+C), with no row cap.
When a worker's result buffer fills, it spills a sorted batch to disk instead of discarding matches.
The batches are combined, identical routes have their counts added, and all results are sorted by
`--sortby` without holding the complete result set in RAM. `--top` limits the console only.
The file begins with comment lines with the command, the start time and (`--stochastic`) the seed; each row's
inputs are on an indented line of their own, so long strings wrap without breaking the columns. The file is
`smartforce-L8-2026-10-24-153207.txt` in the current folder by default, or `--save FILE` (a name ending in
`/` or `\` is a folder for the automatic name; folders are created). It is created when the search starts,
so a bad path is reported at once. An existing file is never overwritten: `-2`, `-3`, ... is added before
the extension. Temporary batches live in a new `FILE.parts` directory beside the output and are removed
after a successful save; a disk error stops the search with a nonzero exit code and retains the batches
for recovery. Saving still uses the existing report, idle-Up normalization and hardfilter rules: strings
merged by `--hardfilter` are not separate results, and routes stop at their deciding frame.
With `--list --save FILE`, every generated string is written without simulation; `--top` still limits
only what appears in the console.

Rows come by frame, then flag. `--sortby Q` (`frame px py vx vy flag`) orders them, the `--stochastic` best
10 and (without `--save`) which rows are kept when memory runs short by that column instead, highest first; `--sortby -Q` lowest
first. Ties go by frame, then flag; for `px py vx vy` dead rows come last.

Simulates every string a pattern expands to and lists those that meet all the conditions. Meant for
routes worked out by hand: fix what is understood, enumerate the rest.

| pattern | means |
|---|---|
| `d18 n3` | runs as in a TAS string (a letter without a count is 1 frame) |
| `d4..7` | every count from 4 to 7 (`d0..3`: the run may be absent) |
| `(A\|B\|C)` | alternatives, each a sub-pattern (they nest) |
| `(...)=6`, `(...)=4..6` | only the group's expansions that are 6 (4 to 6) frames long |
| `<BASE:a3>` | every way to overwrite 3 single frames of BASE (any sub-pattern) with `a`: `<n20:a3>`, `<d18e3:n2>` |
| `<BASE+a3>` | the same, but `a` is added to the keys already held there: `<d20+a1>` puts one `S` (left+right) into `d20` |
| `<BASE:a1..3>`, `<BASE+a0..2>` | every tap count in the range, for either form; 0 includes BASE unchanged |
| `<BASE:a1,w1>`, `<n20:a1..2,w0..1>` | several tap kinds, each with its own count or range, always on different frames (`<<BASE:a1>:w1>` instead lets the `w` overwrite the `a`) |
| `<BASE+a1..5, r>`, `<BASE:a1..5, r>` | reduce one tap kind to one representative per press count and reverse-index sum, using the same placements and order as `j`, without consulting the game state. `+` adds the selected keys; `:` overwrites selected frames. Any input letter is supported; use nested blocks for separate reductions of different kinds. Counts and sums refer to selected placement frames, including frames already holding that key |
| `<BASE+a1..5, r, 30..40%>` | optional index-sum range after `r`, with the same bounds, rounding, and maximum across the whole count range as `j`. The first frame's index is BASE's full length, and the last frame's index is 1. `x%` means `0..x%` |
| `{FILE}`, `{FILE:100}` | every route in FILE (any line that is a whole route: the input lines of a `--save` file, `--list` output), in the file's order, so `{FILE:100}` is a `--save` run's 100 best. Searches can be chained: save the good starts, then `--inputs "{starts.txt}<d12:a0..1>d6..8"` continues each of them; with `--hardfilter`, starts that reached the same state are merged |
| `{FILE}[-N]`, `{FILE}[+N]` | remove the last N frames from each imported route, or append N no-input (`n`) frames. Trimming beyond the start leaves an empty route (`n0` in `--list` output); padding cannot exceed 1,000,000 total frames. `{FILE:100}[-1]` selects the first 100 routes before trimming. Suffixes can be chained, e.g. `{FILE}[-2][+1]`; identical resulting routes are deduplicated as usual |
| `<BASE+j>`, `<BASE+j2..5>` | game-independent Up placements: one representative per press count and reverse-index sum over BASE (frames counted back from its end), instead of every subset. This version does not check whether each Up press actually produces lift; use `+b` for the built-in state-aware model. `<n30+j>` is 4,526 strings instead of 2^30. `j3`: exactly 3 presses; `<BASE:j>`: plain `w` instead of adding Up to the held keys |
| `<BASE:jn, i..j%>`, `<BASE+jA..B, i..j%>` | keep jump placements whose reverse-index sum is between `ceil(i * maximum / 100)` and `floor(j * maximum / 100)`, inclusive, with integer endpoints `0 <= i <= j <= 100`. The maximum is across the entire requested jump-count range, separately for each BASE length L: let K be the largest allowed count that fits L, then `maximum = K*(2*L-K+1)/2`. Indices run from L on the first frame to 1 on the last. For `<d10:j1..2, 25..50%>`, both counts use sums 5 through 9 (maximum 19). `x%` is shorthand for `0..x%`. This limits newly placed presses; existing Up inputs in BASE are unchanged. `0..100%` preserves the original set and order; an interval containing no attainable integer sum produces no strings |
| `<BASE+b10..20, j5..6, 30..40%>` | use the built-in jump brute-forcer from the actual incoming game state, for each window of 10 through 20 frames at the beginning of BASE. Keep BASE's full length, preserve Left/Right, and replace Up inside the window; keep all inputs after it unchanged. `j5..6` allows 5 or 6 generated Up frames. The percentage limits the weighted index sum, using the maximum across the eligible requested press counts for each incoming state and window length. `j` and the percentage can be omitted to allow all candidates |

Place the `b` block **after the take-off input**, as with the viewer's start marker. For example, `e1<d30+b10..20, j5..6, 30..40%>d5` retains the 30-frame base and then continues with `d5`; the initial `e1` is outside the generated window. A window longer than its particular BASE expansion is skipped. BASE can include ordinary sub-patterns or `{FILE}`; multiple `b` blocks can follow one another, each using the state produced by the preceding parts. A `b` block must be a separate pattern part, not nested inside another placement or alternative.

The `b` frame convention matches the viewer: sample `vy` after the window's first native tick, then use weighted indices `N-1, ..., 1` for a window of N frames. Its last input has zero effect on position at that endpoint and receives no generated Up. The model limits the placement window using incoming `vy`, so late ineffective Up padding is not used to represent an earlier lift. Percentages have integer inclusive bounds (lower rounded up, upper down), shared across the entire eligible count range; `40%` means `0..40%`. Existing Up within the window is replaced, so `j` counts the generated frames there; existing Up outside the window remains untouched. The old `+j` / `:j` syntax keeps its previous, game-independent behavior.

The built-in bruteforcer generates representatives using its clear-flight model, rather than every possible input placement. Intermediate collisions can invalidate the model's equivalences. All candidates are replayed by the accepted native simulation; the model does not replace physics or guarantee exhaustive coverage of collision histories.

The `r` modifier is also a representative reduction, not a guarantee of equal game outcomes. Horizontal speed limits, friction, and collisions can make two placements with equal counts and sums behave differently. Generation stays game-independent; every resulting string uses the unchanged native simulation. For example, `<d30+a1..5, r>` generates 400 placements instead of the 174,436 unrestricted subsets, preserving Right and adding Left as `S` on the selected frames.

`b` choices depend on the prefix, so startup totals are **candidate-slot upper bounds**. Deterministic search skips unused slot ranges; stochastic search shuffles slots without repeats and skips invalid choices. `--samples` counts visited slots for these patterns. `--list` also replays prefixes to resolve `b` blocks, and saving still writes every generated result. Matching-branch counts do not multiply an early match by an unknown number of future `b` choices.

At startup, each game-independent jump block prints `jump 1 - min: STRING, max: STRING`, then `jump 2 - ...`, in the order its opening `<` appears in the pattern. The strings are expansions of that block, after its percentage and local length filters, attaining the lowest and highest allowed index sums across its lengths and press counts. They omit the surrounding route. Ties use one representative base string; an empty expansion prints `n0`, and a block with no expansions prints `none`. A state-dependent `b` block instead states that its minimum and maximum depend on the incoming game state. This summary also appears with `--list` and does not add rows to saved route files.

`d18(e1..6d0..5)=6<n4:a1>(d4..7e7..10)=14` is 6 x 4 x 4 = 96 strings (`--list` prints them). `R` is not
allowed.

Strings come shortest first, then in written order (alternatives left to right; in a sequence the first
part changes slowest; a placement's base before its taps). Each part of the pattern counts its strings by
length, so string number i can be built directly from i (unranking). Every part is expanded in memory, with
repeated strings removed, if its predicted size fits in `--maxmem GB` (default 4; about 72 bytes + 1 per
frame per string). A part that does not fit is generated one string at a time in the same order instead:
no size limit (counts up to 4.6e18), but repeated strings (`a1,a1`, or a tap that changes nothing) are
kept and played twice unless `--hardfilter` merges them. The header marks generated parts with `*`.
The `--hardfilter` table (at most 48 bytes per state), the result rows and the game states kept for shared
prefixes (below) also stay within `--maxmem`: when their share is used up, new states are no longer added to
the table, only the best rows are kept (the output says so) and fewer prefixes are kept.
`--maxmem` may not exceed the PC's memory, the threads' game states (about 175 KB per pattern part per
thread) must fit in it, and the counting tables count towards it too. Patterns that would take more than
400 million steps just to count, or brackets nested more than 200 deep, are refused with a message, and
running out of memory stops the search with the results so far instead of crashing.

Conditions (`--report`; default `win`): conditions joined by commas must all hold on the same frame, and
groups separated by `;` are met one after another, each on a later frame than the one before:
`"contact=kickBall@40..60; px>=12.5, ground@80.."` = touch kickBall between frames 40 and 60, later be on
the ground at px >= 12.5 from frame 80 on. The conditions: `win`, `dead`, `alive`, `ground`
(a jump probe touches), `contact`, `contact=NAME` (`--names` lists a level's bodies), and comparisons
`Q<V Q<=V Q>V Q>=V Q=V` with Q one of `px py vx vy vr` (the Practice Hack HUD's and `/set`'s units: metres, m/s,
`vr` = 0.35 x the angular velocity; `px py` are `run`'s x and y), `pr` (rotation in degrees as the HUD shows it: `run`'s angle x 180/pi, fmod 360,
so negative after turning backwards) and `flag` (distance from the ball's box to the flag's box in twips:
the larger of the x and y gaps; <= 0 means touching, minus the overlap depth). Each may have a frame window:
`@F`, `@A..B`, `@..B`, `@A..`. Frames are the game's (the flag frame is the one the game's timer shows).

- Strings that share a prefix share its simulation (the state is copied where the pattern branches),
  also inside a part: each option of a part starts from the state at the longest run of inputs it shares
  with the option before it (`<d76+w2>`: from where its taps first differ). The work is split
  over all threads, each taking a range of options. With `--stochastic`, the states after the first parts
  are kept once per choice of them (up to 256 per thread), so a sample plays only the rest. None of this
  changes a result: the same strings are played and merged, only faster.
- Without `--stochastic`, fixed prefixes repeated across worker tasks also share exact savestates.
  This includes literal strings and each selected string from `{FILE}`, with trimming/padding applied.
  A prefix is cached only when the parts after it have at least 10 combinations and its current part
  contains at least two frames. For example, `{starts}d0..9` qualifies (10 continuations per file row);
  `{starts}d0..8` does not (9). `--pad` adds frames, not combinations. Prefixes already shared within
  one task keep using the existing branch states. The cache is filled as needed, shared by all threads,
  and holds at most `--threads` prefix savestates in total, including entries still being calculated.
  It evicts unused old entries before allocating replacements and stays within the existing 10%
  checkpoint allowance of `--maxmem`; if all slots are in use or there is no room, the task replays
  normally. Existing per-worker branch states are separate. Savestates include report progress and the complete played
  prefix, so saved strings and early matches retain their original counts. `--hardfilter` checks are
  replayed with each task's remaining inputs; `--merge-report` retains the original diagnostic path.
  The final `prefix savestates` line reports entries built, reused, evicted and peak occupancy/limit. The 10x threshold is
  provisional; it has not been tuned by benchmarking.
- A string is decided on the frame its last group holds; the rest of it is not played. It is dropped as
  soon as a window has passed, or when the ball is dead and the groups left cannot hold dead. Strings that
  agree up to the deciding frame are listed once, with their count.
- Up held on a frame where it does nothing is written as the same keys without Up: the ball is awake,
  does not jump (a jump needs a contact and a foot probe) and gets no lift (that needs vy < 0), so the game
  plays both byte-identically. `--idle-up` keeps the strings as written.
- `--hardfilter` merges strings that are in the same game state at the same point of the pattern with the
  same inputs still to play, however different their pasts (inputs that cancel out lead back to the same
  state): only the first is played on and listed, the rest are counted as merged. The check runs between
  the parts of the pattern and every `--merge-every N` frames inside a part (default 32; 0: only between
  parts; 32 found nearly all of 16's extra merges at no extra cost on the Level 8 tests). Every saved result is
  also checked at its deciding frame, including early matches and padding. States are compared without what the game
  never reads again (freed contact slots, manifold points past `pointCount`, the normal of an empty
  manifold, the two speed-up caches, scratch arrays past their counts, the probe and `upIdle` flags), and
  doubles count as equal within `--tolerance T` (default 1e-10, rounded to a grid of that size; `0` =
  bit-exact). With a tolerance the merge is a heuristic, since a 1e-10 difference can grow; on the test
  patterns the merged runs reached the same positions as the unmerged ones. `--merge-report` lists what
  still differs between states whose bodies agree to 1e-7 but that were not merged (`--raw-states`: compare
  the full state, to see what the cleaning gains).
- A string ends where the pattern ends: `--pad N` plays N more `n` frames after it (for example to let
  the ball roll on to the flag).
- Output: the matches by deciding frame (then by distance to the flag), with px, py, vx, vy and the distance
  to the flag on that frame.
- `--stochastic` plays the strings in a uniformly random order without repeats and without storing them:
  string number c is mapped through a keyed bijection of [0, N) (an 8-round Feistel network with
  cycle-walking), so every string is equally likely at every step and a run that finishes has played each
  string exactly once. It runs until every string is played or Ctrl+C, or for `--time SEC` or `--samples N`,
  and every `--update SEC` (default 60, 0: off) prints the time, how many strings it has played and how
  fast (overall and since the last update), then the 10 best strings so far; nothing else is printed while
  it runs. `--seed S` repeats a run (the seed is printed). Above 4.6e18
  strings it picks each part independently at random instead (repeats then possible but vanishingly rare).
  `--hardfilter` works here too: a sample is merged when its state and its remaining inputs match an earlier
  one's. Ctrl+C stops any search and prints the results so far.


- `--start-percent P` starts at `floor(P / 100 * total)` in the numbered random order. Use the same
  `--seed`, pattern, input files and generator version to continue a search. It requires `--stochastic`
  and an explicit seed for a nonzero offset; percentages accept up to nine decimal places. `--samples`
  counts new visits after the offset. This skips enumeration positions, not a percentage of matches,
  and does not restore a previous run's merge table.
- `--exclude FILE` skips exact decoded strings already in a result file before simulation. It works
  with random searches and listing; it does not exclude equivalent states or all variants of a prefix.

## Architecture

| file | contents |
|---|---|
| `src/b2math.h` | AS3-exact vector/matrix/sweep math; `Number.MIN_VALUE` epsilons; the single `as3_sin/as3_cos` choke point |
| `src/libm_intel.S` | Flash Player 11.4's `sin`/`cos` (Intel LIBM, generated by `tools/hotspot2gas.py`) |
| `src/b2collision.cpp` | polygon geometry, circle/poly narrow phase, GJK distance, TOI |
| `src/b2world.cpp` / `b2world.h` | SAP broadphase, pair manager, contact lifecycle and listener emulation, island + contact solver, TOI loop, bodies/shapes, filtering |
| `src/b2joints.cpp` | distance, prismatic (limits, motors) and revolute (motors) joints |
| `src/redball.cpp` / `redball.h` | base `Level` logic (construction, sprite sync, probes, input, camera, display layer, spikes, death and post-death update), the 17 level scripts, RLE codec |
| `src/levels_data.h` | named placements of all 17 levels (`tools/extract_levels.py`, `tools/gen_levels_data.py`) |
| `src/level_polys.h` | polygon tables of levels 9-17 as written in the AS3 (`tools/gen_level_polys.py`) |
| `src/display_data.h` | bounds of every named object and every spike, per level (`tools/gen_display_data.py`, `tools/swf_geom.py`) |
| `src/flash_sintab.h` | Flash's fixed-point sine table (`tools/gen_flash_sintab.py`; identical to the player's) |
| `src/snapshot.cpp` / `snapshot.h` | compact, portable, byte-exact snapshots |
| `src/smartforce.cpp` | `rbsim smartforce` |
| `src/verify.cpp` / `src/calib.cpp` | stats-log replay / calibration-dump checks |
| `src/rbsim.cpp` | CLI, self-tests, benchmark |
| `tools/deathcause.cpp` | replays inputs and prints each death's cause (contacts or spike row) and the win |
| `docs/mod_sweeps.as` | AS3 for the logging mod's E11/E12 calibration sweeps |
| `docs/FLASH_PLAYER_RE.md` | what the decompiled Flash Player 11.4 does (addresses, formulas, the shape hit test) |
| `viewer/rbview.cpp` (+ `.rc`, `.ico`, `.manifest`) | Red Ball TAS, the Windows program (`make viewer`) |
| `viewer/tas.cpp`, `tas.h`, `tas_test.cpp` | its TAS core (the mod's keys as operations on a route), tests (`make tastest`) |

All mutable state lives in fixed arrays linked by `int` indices, so `Sim copy = sim;` is a complete,
bit-exact snapshot (175 KB). Immutable geometry is shared; scratch buffers are per thread (fiber-local
storage on Windows), so one sim per thread is safe.

## What is modelled beyond the physics

Details and numbers: `docs/STATS_LOGGING.md` §3.5-3.11.

- **Display matrix:** built from the value *written* to `rotation` (16.16 fixed-point degrees) with
  the player's own fixed-point sine/cosine (0.25-degree table, quarter-weight interpolation bug),
  ported from the decompiled player; exact on 26,716 logged matrices. Timeline-placed rotated clips
  get the rotation the player derives with its fixed-point atan2 (reproduces all 46 values measured
  in dumps, so new rotated clips need no E9a dump).
- **Hit tests:** `hitTestObject` on twip bounding boxes (touching edges count); rotated boxes come from
  the player's float32 corner transform (round half to even). A turned multi-part clip (Level 11's pushed flag: pole +
  cloth) is boxed per part with round-to-nearest corners, then unioned (E12: 148,074/148,074).
  `getBounds` of a nested sprite follows a different (union) rule, so the two are not the same call.
- **Camera:** Tweener easeOutExpo at t = 1 of 31 frames per `Update`, truncated to twips; `dp` is the
  camera step of the frame.
- **Real-game spikes** (`MATHSPIKES = 0`, original game, Tournament Edition): Flash's renderer hit test in window
  subpixels (whole-pixel probe, float32 device matrices, fuzzy curve crossings, box padded by 2 subpixels on the
  right/bottom) against each spike's edges **as of the previous frame's redraw**, the source of window-dependent
  deaths. `--window WxH [--noscale]`; see `docs/RESOLUTION.md`.
- **Standardized spikes** (`MATHSPIKES = 1`): 16 control points, `getBounds` test, shift by `-dp`
  (the **spike glitch**; `--gless` for glitchless), strict triangle test with Flash's twip
  conversions. Turned/scaled spikes: Flash's `globalToLocal` works in stage twips (camera
  included) with float32 matrices (SWF >= 8): the player's float32 inverse and point transform are
  ported exactly (E11: 390,670/390,670).
- **Death:** `PlayerDie` destroys the ball's body and the level keeps running its full `Update`
  (world step, moving parts, level script); the camera follows the frozen ball and the goal,
  checkpoint and switch tests (no `IsLive()` guard) compare the ball, now off the display list, with
  targets shifted by the camera: the **death warp**. Real-game timers are a route rule (flag no later
  than death + 38 frames; unpause at flag + 88).
- **Static level flags** (`Level_7.redCheckLevel`, `Level_13.greenCheckLevel`, `Level_16.isStrelka`)
  survive checkpoint restarts.
- **Not simulated:** the 8 death-debris bodies (random positions, so a route that relies on them is
  not reproducible in the real game either); pausing (the TAS hack has none).

## Port quirks replicated (differences from stock Box2D 2.0 C++)

1. Pair buffer is **not sorted** before commit, so contacts are created in insertion order, which
   drives solver order.
2. Contact velocity bias for separated points is a hard-coded `-60 * separation`.
3. The friction clamp uses the normal impulse from **before** the current iteration's update.
4. Restitution bias is computed from pre-warm-start velocities.
5. `Number.MIN_VALUE` (a denormal) is used everywhere C++ used `FLT_EPSILON`.
6. Uninitialised `Number` fields are **NaN** (manifold impulses/separation, `m_toi`, TOI fields).
7. Bodies are created static, so `SetMassFromShapes` flips their type and recreates proxies (new
   ids, new pair order).
8. Game listener bug: `playerContactBodies.splice(body, 1)` always removes index 0; Add/Remove fire
   per manifold point and on feature-key changes, so the list can hold stale or duplicate entries.
9. Destroyed bodies remain readable (the game keeps probing a dead ball). Their slots are reused only when
   all are taken (a drop platform re-created on every frame it is touched), and only for bodies destroyed
   before the last step that nothing refers to any more.
10. Decompiler loss: JPEXS dropped the `bestEdge/bestSeparation` update in `FindMaxSeparation`; the
    AVM2 p-code has it and the port includes it.

Joint code: every field write in all 48 joint methods was cross-checked against the AVM2 p-code.
Only distance, prismatic and revolute joints are ever created by the game.

AS3 quirks of the level scripts are reproduced too, e.g. Level 17's second platform starts with
direction 0 (never initialised), Level 16's wrong-way trigger resets the checkpoint (also after
death), Level 15's loose plank exists only from checkpoint 0, Level 11's flag is a physics body.

## Verification status

Every field of every frame is compared bit-for-bit against stats logs recorded with the logging mod
described in `docs/STATS_LOGGING.md` (Flash Player 11.4.402.287, Windows XP, x86). All logs below
match on every frame compared.

| level | logs | frames | deaths on Flash's frame | notes |
|---|---|---|---|---|
| 1 | 2 | 1,204 | — | incl. the Level 1 TAS (win at tick 510) |
| 2 | 8 | 23,729 | 1 | 18,595 of them after a death (camera, `dp`, frame counter) |
| 3 | 5 | 2,329 | 10 | spike deaths, 2 wins, checkpoint restart |
| 4 | 2 | 3,365 | 2 | death-warp TASes: any% (flag 274), delayed warp (flag 309) |
| 5 | 4 | 4,626 | 1 | blue switch, 2 wins |
| 6 | 4 | 3,320 | 1 | drop platforms, checkpoint restart, win, spike death |
| 7 | 5 | 4,426 | 2 | red switch carried across R, blue switch, 2 wins |
| 8 | 12 | 10,009 | 7 | car, crushers, ramps, wall spikes, double death warp (checkpoint 200, flag 1084), 5 wins |
| 9 | 7 | 7,775 | 7 | spike, roll-ball and boom-crank deaths, green switch, crank riding, checkpoint-3 R and win (224), death-warp win (death 358, flag 359) |
| 10 | 4 | 6,241 | 6 | fall deaths (incl. after R), `roundBlock` death, jump platforms, cubes, `goBall`, TAS win (394) |
| 11 | 5 | 4,507 | 3 | train, `killRotate` and turned-spike deaths, the flag pushed and turned by the train, a death warp onto the still-moving flag (death 1110, flag 1117), win (309) |
| 12 | 5 | 5,006 | 2 | collision groups, checkpoint-2 restart, 2 wins |
| 13 | 4 | 4,022 | 5 | the loose star killing an idle ball, `kingStar1`/`killStar2` deaths, R from checkpoints 1 and 2, 2 wins |
| 14 | 3 | 2,931 | 1 | switch, drop platform, checkpoint restart, win |
| 15 | 4 | 3,573 | 4 | `killLine` deaths after R from checkpoint 2, red switch (gate rebuilt as a spinning door), skateboard, 2 wins |
| 16 | 5 | 4,414 | 3 | roll-ball death, drop platform, wrong-way trigger (alive and as a death warp), 2 wins; one route recorded with `isGless` off (survives a spike by the spike glitch) and on (dies there) |
| 17 | 4 | 7,693 | 9 | fall, `killStarPart` and 15-degree spike deaths, R from checkpoint 1, TAS win (338) |

The ball's display matrix, the camera, `dp`, `lastCheckNum` and the level flags are compared on every
frame. After deaths the level's bodies are compared too; they differ only where the random debris hit
something. Logger artifacts handled by `verify`: death and win rows log input 0 (`OutControl`) and are
retried with the previous input; rows logged after the frame counter stops are skipped; the frame
counter continues across `R`.

Not exercised by any log (same code paths as logged parts): the Level 5/13 green switches, the Level 16
blue switches, a few kill bodies (Level 11 stars and logs, Level 16 axe), some checkpoints as restart
points.

Calibration results:

| question | answer |
|---|---|
| twip quantisation of `x`/`y` | truncation toward zero (2,400/2,400) |
| `rotation` getter | written value, `fmod`-normalised to (-180, 180] (720/720); timeline clips: the player's fixed-point atan2 of the matrix (46/46 measured values) |
| display matrix from `rotation` | the player's fixed-point trig, ported (26,716/26,716 logged) |
| x87 extended precision in physics | no; all arithmetic matches SSE2 doubles |
| `Math.sin`/`cos` (FP 11.4) | Intel LIBM (0/4,096 mismatches; the player's `_CIsin` jumps to `__sin_pentium4`) |
| `localToGlobal` / spike `testPoint` | Flash's twip truncation and rounding (plain spikes: 16,000 + 26,040 + 30,000 cases) |
| turned/scaled spikes (E11) | 390,670 / 390,670 (float32 inverse ported from the player) |
| turned Level 11 flag (E12) | 148,074 / 148,074 |
| real-game spikes (`MATHSPIKES 0`) | still sweeps at 7 window sizes: E8c 240,000 / 240,000, E11 1,172,010 / 1,172,010; moving camera: per-frame edge scans 390 / 390, 28 real-time playbacks at 7 window setups bit-exact to the death frame |
| placements and bounds (E9a/E10) | every dumped level (5, 8-17): all placements and every hit-test target exact; a few nested scaled walls that are never hit-tested are 1 twip off (Flash's rounding of nested rotated+scaled bounds is not modelled) |

An older custom ActiveX host (Flash 11.5) has different `sin`/`cos` (1 ulp on ~3.5% of inputs): never
use logs from it.

**Platform note:** x86-64 only (Linux, Windows). macOS x86-64 would need the assembly's
section/symbol directives adapted; ARM (incl. Apple Silicon) cannot run the Intel `sin`/`cos` routine.

**Licensing note:** `src/libm_intel.S` is derived from OpenJDK HotSpot code, which is GPL-2.0-only
(Intel copyright). Binaries that include it fall under GPLv2. The existing notice and license text
are retained in [`LICENSES/`](LICENSES/README.txt). No project-wide license has been selected for the other code.

## Roadmap

1. **Viewer:** `rbview.exe` is the TAS tool (the mod's keys) plus viewer, states, ghost. Possible next:
   an input piano roll (edit frames by clicking), a real-Windows check of the keyboard timing.
2. **Simulator:** complete; the display layer is the player's own arithmetic, including the real game's
   shape spike test at any window size (`MATHSPIKES 0`, `docs/RESOLUTION.md`). Open: confirm it with one stats
   log recorded on real Windows; the Tournament Edition's Level 15 money tween (0.3 s of real time).
