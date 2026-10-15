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
| H / Shift+H / J | HUD / spike glitch marks (where each spike really kills this frame) / physics outlines + the ball's 16 control points |
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
  touches the flag, that is a death warp. Mouse wheel zooms; dragging pans (*Follow camera* goes off).
- **Tabs on the right:**
  - **Events:** deaths, checkpoints, switches, restarts, warps and the flag frame. Click one to go there.
  - **States:** the ten slots (0-9), saved states with names and no limit, and the backups: every route that
    R, a rewind or a load replaced (the last 500). All of it is kept in `rbview_states.txt` next to the exe.
  - **Ghost:** a second route drawn as a blue ball on the same frame: the current route (as it is now), a pasted
    string, a file, or a saved state. The tab compares the flag frames.
  - **Search:** rbsim's searches inside the program. *Optimize* improves the whole route for the chosen time;
    *Beam* searches on from the shown frame. Stop ends early. **Load result** puts the best route found in the
    editor (the old one goes to the backups).
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
  them. Routes with warps or invincibility are marked (`[tool]`, and on the HUD) and the search refuses them.
- Space while the game runs pauses it (the mod ignores it). The end of a playback keeps your game speed (the mod
  resets it to 1x), and you can choose to pause there.
- R, F, I, O, Backspace and B also work while paused in the middle of a playback. They act at the shown frame;
  the rest of the route goes to the backups.

Settings are kept in `rbview.ini`, and states and backups in `rbview_states.txt`, both next to the exe.

Colours: dark grey = static ground, blue = moving parts, dark red = things that kill, red triangles = spikes,
orange = where a spike really kills this frame (Shift+H), cyan boxes = checkpoints (filled once collected), gold
box = flag, blue/green/red boxes = switches (filled once pressed), orange box = Level 16's wrong-way trigger.

## Command-line tool: `rbsim.exe`

The rest of this guide is for running the simulator and the route search from a command prompt.
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

Typing `rbsim.exe` alone lists the commands; `rbsim.exe optimize` alone lists its options.

## 2. Searching for a faster route

```
rbsim.exe optimize --level 8 --inputs "d19a3n1a1n12d1a1n21d15e1d1e1d3e22d7e5d30e1d20e7d3e1d9n1d76e2d14a1n1a1n18a3n10a1n18w9n65" --time 600
```

- `--level N`: the level (1-17).
- `--inputs "..."`: the route to improve, in the TAS hack's string format (`n d w e a S q W`
  plus counts). It must collect the flag. Remove any `R` (one attempt only).
- `--time SEC`: how long to search, in seconds (default 30). `--time 3600` is an hour.
- `--threads N`: how many CPU threads to use (default: all of them). Use fewer if you want to
  keep using the PC while it runs, for example `--threads 4`.
- `--checkpoint C`: start from checkpoint C instead of the level start.
- `--gless`: glitchless mode (no spike glitch). Leave it out for the normal TAS rules.
- `--mathspikes 0 --window WxH` (add `--noscale` for the Practice Hack's default scale mode): optimize for the
  original game's / Tournament Edition's spikes at that window size. The result is only valid there.

While it runs, every improvement is printed as soon as it is found, for example:

```
  frame 403, margin 12 twips  d19a3n1...
```

At the end it prints a summary and the best route as a string you can paste back into the TAS
hack. Frame counts are converted to seconds at 31 fps.

- **Stopping early:** press `Ctrl+C`. The summary is not printed then, but every improvement was
  already printed above, so copy the last `frame ...` line.
- **Death warps:** if the best route finishes with a death warp, the output also gives the frame
  by which you must pause and the frame on which to unpause in a real run.
- **"check this route in Flash":** printed when a hit test on the route was within one rounding
  unit of going the other way. Play that route in Flash before trusting it.
- **Same result on any PC:** with `--threads 1 --seed 5 --evals 200000` (stop after a fixed number
  of candidates instead of a time) the output is identical on every machine, Windows or Linux.
  Useful for comparing results or reporting a problem.

Save the output to a file by adding `> result.txt` at the end of the command.

## 3. Finding a route without a known one (beam search)

```
rbsim.exe beam --level 4 --memory 10G
```

Keeps the best states after every frame and expands them with all inputs, starting from the
level start (`--checkpoint C`: a checkpoint; `--prefix "..."`: after some inputs). It prints a
progress line every few seconds and the route at the end. `--memory` is the RAM it may use
(default 4G); leave room for Windows. `--seed-route "..."` keeps a known route in the beam so the
result can only match or beat it.

Long runs save progress to a folder (`beam_L4` here) every 10 minutes. If the PC restarts or
you press `Ctrl+C`, run the same command again with `--resume` added to continue. The result is
the same as an uninterrupted run, on any PC.

## 4. Checking a recording against the simulator

If you recorded a stats log or calibration dump with the logging mod (`docs/STATS_LOGGING.md`):

```
rbsim.exe verify C:\logs\rb1_stats_L7_1.tsv
rbsim.exe calib C:\logs\rb1_calib_L7.tsv
```

`verify` replays the log and compares every frame. The line to look at is
`segments: N (bit-exact to end/death/win: N, diverged: 0, ...)`: `diverged: 0` means the
simulator matches Flash exactly. `calib` ends with `CALIB OK` or `CALIB MISMATCH`.

If a path contains spaces, put it in quotes: `rbsim.exe verify "C:\My Logs\run 1.tsv"`.

## 5. Other commands

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
(`optimize` results) is fine either way.

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
