# Running rbsim on Windows

## Red Ball TAS: `rbview.exe` (no command line)

**[Download `rbview.exe`](https://github.com/Theme25/project-unnamed/releases/latest/download/rbview.exe)
and double-click it.** It is one file: no installer, nothing else to install, no command prompt.
If Windows says "Windows protected your PC" (the exe is not signed), click **More info**, then
**Run anyway**.

`rbview.exe` is the Practice Hack's TAS mode on the bit-exact simulator: **the same keys and the same
behaviour as the mod**, with a proper UI around it, and a route viewer. Click the level view, press **T** to run,
and play with A/W/D (or the arrows). Everything you play is recorded into the route string at the top.

### Keys (as in the Practice Hack)

| key | what it does |
|---|---|
| A / W / D (or Left / Up / Right) | move. A key tapped between two frames still counts for the next frame |
| T (or P) | pause / run |
| Space | one frame (if the game is running, the first press pauses it). Hold it to keep stepping, even while you press other keys. Shift+Space: remove the last frame |
| , and . | game speed down / up: 0, 1/32 ... 1 ... 64 |
| 0-9 | save the route up to the shown frame into state 0-9. Shift+0-9: load it |
| I, then 0-9 | import the clipboard (a route string) into a state. Shift+0-9: import and play it |
| O, then 0-9 | copy a state's string to the clipboard. Shift+O: the backups |
| Backspace | remove the last 10 frames (Shift: 31), while paused |
| Esc | drop the rest of a playback: the route ends at the shown frame (paused there) |
| R / F | restart the level (empty route) / checkpoint restart (adds `R` to the route) |
| U | real-time state loads: a loaded state plays back instead of appearing at once. Shift+U: fast playback |
| G / Shift+G | invincibility / glitchless |
| Shift+J | jump probes on/off (rbview): the 3 ground tests under the ball, green when touching (the HUD lists them too) |
| H / Shift+H / J | HUD (grows with the window; includes the distance to the flag) / graphics marks: each spike's effective hitbox this frame, the ball's 16 control points / physics outlines + the ball's 16 control points |
| B / Shift+B | the jump brute-forcer: B on the take-off frame, Shift+B on the landing frame (then W/S, Space, Enter, Esc) |
| Ctrl+A / Ctrl+D | previous / next level |
| / | the command line: `/config mathspikes 0`, `/config decimals 6`, `/set -vx 5 -vy -3 -vr 0 -px 12 -py 8`, `/remap a left`, `/remap reset` |
| Z / Shift+Z / = | zoom in / out at the mouse / reset |

rbview only: **Home / End** first / last frame, **PageUp / PageDown** one frame back / on (Shift: 10).
These only move the view: nothing in the route changes. **F1** shows the keys. **Keys...** remaps any
key (the mod's `/remap`). While you type in a text box (the route, the command line, a name), the keys do nothing:
digits typed into the route do not save states. Click the level view to play again.

### The window

- **Top row:** level, start (level start or a checkpoint), **Glitchless**, **Invincible**, and the spike rule.
  Keep *Standardized (mathspikes 1)* for the speedrun rules. *Flash shape test (mathspikes 0, TE)* is the original
  game and the Tournament Edition. There, deaths depend on the size of the Flash area (**Window**: the projector
  window without its menu bar; pick a preset or type a size such as `935x925`) and on **Scale** (*Show all* = the
  projector's View > Show All).
- **Route:** the input string, kept up to date while you play. Type or paste a string and press Enter (or **Load**)
  to load it at frame 0. **Paste / Copy** use the clipboard. **Open...** (or drag a file onto the window) reads a
  route file or a stats log (`.tsv`) from the logging mod: level, checkpoint, glitchless and the spike rule, window
  and scale (its `#ENV` line) come from the log. **Save...** writes the route with its settings.
- **Transport:** |< < Play > >|, the **Speed** slider (0x, 1/32x ... 1x ... 64x; also , and .), *Follow camera*, and
  the slider to look at any frame.
- **Command line** (**Run command**) for the mod's commands. Everything it can do is also in the Options tab. Next
  to it: the mode (RECORDING, PLAYBACK, PAUSED, BRUTE FORCE) and the mod's messages.
- **Level view:** the level, moving parts, spikes, checkpoints, switches, the flag and the ball with the game's camera.
  The played path is red, the rest of the route (still to play back) is faint. The HUD shows the mod's numbers
  (time, speed, velocities, rotation, position, contacts) and the keys of the frame. After a death a dashed purple
  **warp ghost** shows where the game tests the dead ball against the flag, checkpoints and switches: when it
  touches the flag, that is a death warp. The view follows the game camera by default, also while zooming (mouse wheel, Z). Dragging pans and leaves *Follow camera*; Play, loading a route and = go back to it.
- **Tabs on the right:**
  - **Events:** deaths, checkpoints, switches, restarts, warps and the flag frame. Click one to go there.
  - **States:** the ten slots (0-9), saved states with names and no limit, and the backups: every route that
    R, a rewind or a load replaced (the last 500). All of it is kept in `rbview_states.txt` next to the exe.
  - **Ghost:** a second route drawn as a blue ball on the same frame: the current route (as it is now), a pasted
    string, a file, or a saved state. The tab compares the flag frames.
  - **Options:** every setting the mod has as a key or a command, as checkboxes and sliders.
    - *Playback:* real-time state loads (U), fast playback (Shift+U), and what happens when a playback reaches the
      end of the route: pause there, or keep running and record your keys (the mod's behaviour). After the flag the
      run is over, so the clock always stops there.
    - *Display:* HUD (H), spike glitch marks (Shift+H), physics outlines (J), path, HUD decimals.
    - *Warp:* the ball's velocity, rotation and position (filled in from the ball), *Warp at this frame* (the mod's
      `/set`), *Remove all warps*.
    - *Keys and remapping* (the mod's `/remap`).
  - **Brute force:** the mod's jump brute-forcer. Every way it finds is also simulated, so the list shows the real
    end height and speed next to the mod's estimate.

### Differences from the mod

- A key tapped between two frames is recorded in the route. The mod applies such a tap but does not write it into
  its string, so the string does not replay the same.
- Glitchless and invincibility apply to the whole route: changing them re-simulates it. In the mod they switch at
  the moment you press the key.
- `/set` warps are stored in the route, so states with a warp replay correctly in rbview. The game cannot replay
  them. Routes with warps or invincibility are marked (`[tool]`, and on the HUD).
- Space while the game runs pauses it (the mod ignores it). The end of a playback keeps your game speed (the mod
  resets it to 1x), and you can choose to pause there.
- R, F, I, O, Backspace and B also work while paused in the middle of a playback. They act at the shown frame;
  the rest of the route goes to the backups.

Settings are kept in `rbview.ini`, and states and backups in `rbview_states.txt`, both next to the exe.

Colours: dark grey = static ground, blue = moving parts, dark red = things that kill, red triangles = spikes,
orange = a spike's effective hitbox this frame: where a control point (magenta) kills, the outline shifted by the camera step and cut to the spike's box (Shift+H; the spike itself turns pale). Under mathspikes 0 it is drawn as the exact set of window pixels that kill (Flash tests each control point on a whole window pixel), so with the window size and camera of that frame a magenta point inside an orange cell kills and one outside does not; the three rings under the ball are the jump probes (green = touching, so up jumps; Shift+J), cyan boxes = checkpoints (filled once collected), gold
box = flag, blue/green/red boxes = switches (filled once pressed), orange box = Level 16's wrong-way trigger.

## Command-line tool: `rbsim.exe`

The rest of this guide is for running the simulator from a command prompt.
You do not need to build anything if you have the ready-made `rbsim.exe`. To build it yourself,
see the end of this page.

Requirements: 64-bit Windows 10 or 11 on an Intel or AMD processor. ARM laptops (Snapdragon)
are not supported. Nothing else needs to be installed.

## 1. First run

1. Put `rbsim.exe` in its own folder, for example `C:\rbsim`.
2. Open that folder in File Explorer, click the address bar, type `cmd` and press Enter.
   A Command Prompt opens in that folder. (PowerShell works too; see "Quoting" below.)
3. Check that everything works:
   ```
   rbsim.exe test
   ```
   The last line must be `ALL PASSED (0 failures)`. If it is not, stop and report it: the
   results on this machine cannot be trusted.

**"Windows protected your PC" / SmartScreen:** the exes are not signed, so Windows may warn the
first time. Click **More info**, then **Run anyway**. Some antivirus programs also flag unsigned
tools; if yours deletes or quarantines `rbsim.exe` or `rbview.exe`, add the folder as an exception.

**"'rbsim.exe' is not recognized":** the Command Prompt is not in the folder containing the exe.
Use `cd C:\rbsim` first, or give the full path: `C:\rbsim\rbsim.exe test`.

Typing `rbsim.exe` alone lists the commands; `rbsim.exe smartforce` alone lists its options.

## 2. Trying many routes at once

```
rbsim.exe smartforce --level 2 --inputs "d18(e1..6d0..5)=6<n4:a1>(d4..7e7..10)=14" --report "contact=kickBall@40..60"
```

- `--inputs "..."`: a pattern. Plain runs as in the TAS string, plus `d4..7` (every count from 4 to 7),
  `(A|B)` (alternatives), `(...)=6` (only the group's expansions that are 6 frames long), `<BASE:a3>`
  (every way to overwrite 3 single frames of BASE with `a`; BASE can be any pattern, e.g. `<n20:a3>` or
  `<d18e3:n2>`) and `<BASE+a3>` (the same, but `a` is added to the keys held there: `<d20+a1>` puts one
  `S` into `d20`). A range such as `<n20:a1..3>` or `<d20+a0..2>` tries every tap count in it, and
  `<n20:a1,w1>` places one `a` and one `w` on two different frames. Any part of the pattern can be as
  large as you like: parts that fit in `--maxmem` gigabytes (default 4) are kept in memory, larger ones are
  built one string at a time. The example is 6 x 4 x 4 = 96 strings; add `--list` to print them without simulating.
- `--report "..."`: what a string must do, comma-separated: `win`, `dead`, `ground`, `contact=NAME`
  (`rbsim.exe smartforce --level 2 --names` lists the names), or `px>=12.5`-style comparisons on
  `px py pr vx vy vr flag` (in the HUD's units, as `/set` takes them; `pr` = rotation in degrees; `flag` = distance to the flag in twips, 0 or less
  means touching). Add `@F` or `@A..B` for a frame window. Default: `win`.
- `--pad N`: N extra frames of no input after each string (the pattern's end is the string's end).
- Up held where it does nothing (in the air and falling, or on the ground without a jump) is shown as the
  same keys without Up, since the game plays both the same. `--idle-up` keeps the strings as written.
- `--hardfilter`: strings that end up in the same game state are only played once; the output says how many
  were merged. Numbers closer than `--tolerance` (default 0.0000000001) count as equal; `--tolerance 0`
  merges only exact copies. States are compared between the parts of the pattern and every 32 frames
  inside a part (`--merge-every N` to change, 0 = only between parts).
- `--stochastic`: for patterns too big to play in full. Plays the strings in random order (never the same
  one twice) until all are played or you press Ctrl+C (or for `--time SEC` / `--samples N`).
  Every minute (`--update SEC` to change, 0 = off) it prints how many strings it has played and how fast,
  then the 10 best strings so far, so a long search can be checked while it runs. `--seed S` repeats a run.
- `--save` writes all the matching strings and their data to a new file at the end
  (`smartforce-L8-2026-10-24-153207.txt`), or `--save mysearch.txt` to choose the name. Nothing is
  overwritten: a second `mysearch.txt` becomes `mysearch-2.txt`. The file also records the exact command,
  so the search can be repeated.
- `--sortby px` lists the matches (and the best 10 of `--stochastic`) by px, highest first; `--sortby -px`
  lowest first. Also `frame py vx vy flag`. The default is earliest frame, then smallest flag distance.
- `--checkpoint C`, `--gless`, `--threads N`, `--top N` (how many matches to print, 0 = all). Ctrl+C stops
  a search and prints what it found so far.

It prints the matching strings sorted by the frame on which the conditions were met, with the ball's
position, speed and distance to the flag on that frame. Frames are the game's, as on the game's timer.

## 3. Checking a recording against the simulator

If you recorded a stats log or calibration dump with the logging mod (`docs/STATS_LOGGING.md`):

```
rbsim.exe verify C:\logs\rb1_stats_L7_1.tsv
rbsim.exe calib C:\logs\rb1_calib_L7.tsv
```

`verify` replays the log and compares every frame. The line to look at is
`segments: N (bit-exact to end/death/win: N, diverged: 0, ...)`: `diverged: 0` means the
simulator matches Flash exactly. `calib` ends with `CALIB OK` or `CALIB MISMATCH`.

If a path contains spaces, put it in quotes: `rbsim.exe verify "C:\My Logs\run 1.tsv"`.

## 4. Other commands

```
rbsim.exe run --level 2 --inputs "d18e1w1n1w5" --every 10
rbsim.exe log --level 2 --inputs "d18e1w1n1w5" > sim_log.tsv
rbsim.exe bench
```

- `run` prints the ball's state each frame (`--every 10`: every 10th frame; `--checkpoint K`:
  start at checkpoint K; `--hex`: exact bit patterns).
- `log` writes the simulator's own log in the mod's format (here into `sim_log.tsv`).
- `bench` measures the simulation speed of this PC.

## Quoting: Command Prompt vs PowerShell

Both work with the commands above as written. Always put the input string in double quotes.
In PowerShell, run the exe as `.\rbsim.exe` (with `.\` in front) when you are in its folder.

Saving output with `>`: in Command Prompt it just works. Windows PowerShell 5.1 (the version
built into Windows) writes `>` files as UTF-16, which `rbsim.exe verify` cannot read back. For
files you will feed to rbsim again (like `log` output), either use Command Prompt or write
`| Out-File -Encoding ascii sim_log.tsv` instead of `> sim_log.tsv`. Text you only read
(`smartforce` results) is fine either way.

## Building it yourself (optional)

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 UCRT64** shell from the Start menu.
2. Install the compiler and tools:
   ```
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc make git
   ```
3. Get the source and build:
   ```
   git clone <repository URL>
   cd project-unnamed
   make
   ./rbsim.exe test
   ```
   This produces a standalone `rbsim.exe` that can be copied to any 64-bit Windows PC.

Do not change the compiler flags in the `Makefile`: other flags (for example `-O3
-march=native`) were tested and break the exact match with Flash. Visual Studio cannot build
the project.
