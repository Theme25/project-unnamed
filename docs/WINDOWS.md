# Running rbsim on Windows

## Route viewer: `rbview.exe` (no command line)

**[Download `rbview.exe`](https://github.com/Theme25/project-unnamed/releases/latest/download/rbview.exe)
and double-click it.** It is one file: no installer, nothing else to install, no command prompt.
If Windows says "Windows protected your PC" (the exe is not signed), click **More info**, then
**Run anyway**. It plays a route through the bit-exact simulator and draws the level,
the moving parts, spikes, checkpoints, switches, the flag and the ball, with the game's camera.

1. **Level** and **Start from** (the start or a checkpoint), **Glitchless** if the route is
   meant for the spike glitch off.
2. **Route:** paste the input string (`d12e1w3n5...`, the TAS hack's format) and press **Load**
   (or Enter). **Open...** (or drag a file onto the window, or onto `rbview.exe`) loads a `.txt`
   with the route or a stats log (`.tsv`) from the logging mod: level, checkpoint and glitchless
   mode are then taken from the log.
3. **Play** (Space). **<** and **>** (Left/Right arrow) step one frame; **|<** / **>|** (Home/End)
   jump to the start/end; the slider seeks anywhere; **Speed** from 0.1x to 8x (1x = the game's
   31 frames per second).
4. The list on the right shows what happens and when: deaths, checkpoints, switches, restarts,
   the flag frame (and whether a death-warp finish is still valid in a real run). Click a line
   to jump there.
5. Mouse wheel zooms; dragging pans (this turns off **Follow camera**; tick it again to follow
   the game's camera). **Show path** draws the ball's path.

After a death, a dashed purple **warp ghost** shows where the game tests the dead ball against the
flag, checkpoints and switches (shifted by the camera): when the ghost touches the flag, that is a
death warp. The 8 random debris pieces are not shown. The window remembers the last route in
`rbview.ini` next to the exe.

Colours: dark grey = static ground, blue = moving parts, dark red = things that kill, small red
triangles = spikes, cyan boxes = checkpoints (filled once collected), gold box = flag, blue/green/red
boxes = switches (filled once pressed), orange = Level 16's wrong-way trigger.

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
