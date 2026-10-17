# Spikes at any window size (MATHSPIKES 0, the original game, the Tournament Edition)

**Short version.** The original Red Ball 1 spikes, the Tournament Edition (`te.swf`) and the Practice Hack's
`/config mathspikes 0` all kill with `hitTestPoint(x, y, true)`. That is Flash's renderer hit test: it runs in
window pixels, so the outcome depends on the window size, the scale mode, the camera and, while the camera
moves, on the previous frame's redraw. rbsim now models it bit-exactly:

```
./rbsim run    --level 3 --inputs RLE --mathspikes 0 --window 935x925            # showAll (the default)
./rbsim verify rb1_stats.tsv                                                      # window/scale from the #ENV line
./rbsim beam   --level 3 --mathspikes 0 --window 1280x720 --noscale ...           # any command takes the three options
```

In `rbview.exe`, use the **Spikes**, **Window** and **Scale** boxes on the top row. A stats log opened in the
viewer sets all three from its `#ENV` line.

The other two rules do not depend on the window. **MATHSPIKES 1**, the standardized check and the speedrun rule,
was checked on dumps at five window sizes. **MATHSPIKES 2** never touches display coordinates
(`docs/MATHSPIKES_RESOLUTION.md`). Routes found under them are unaffected by this work.

| rule | used by | depends on the window |
|---|---|---|
| `--mathspikes 1` (default) | Practice Hack default, speedruns | no |
| `--mathspikes 2` | Practice Hack `/config mathspikes 2` | no |
| `--mathspikes 0 --window WxH [--noscale]` | original game, Tournament Edition, Practice Hack `/config mathspikes 0` | **yes** |

## 1. What "window" means

`--window` is the size of the **Flash area**: the projector's client area without its menu bar. It is the
`window` value in the mod's `#ENV` line (`Shipik.envLine` reads it by switching to noScale for an instant).
The scale mode is `showAll` unless `--noscale` is given. The Tournament Edition sets `showAll` itself. The
Practice Hack starts in noScale, and the projector's *View > Show All* switches it. Quality is assumed `HIGH`;
both SWFs set it.

## 2. The model (each step is the player's own code, Flash Player 11.4.402.287)

Units: **subpixels** = window pixels × 4. At quality HIGH the player's anti-aliasing factor (field +0x48) is 4.

1. **Window → stage scale** (`pix::MakeWindow`).
   - showAll: `kx = f32(W/11000)`, `ky = f32(H/8000)` (twips → pixels).
     - If `W == floor(1.375·H + 0.625)` and `8W ≠ 11H`, the player keeps the two scales separate and adds
       no offset (the "hidden" sizes).
     - Otherwise both scales become `k = min(kx, ky)`, and the letterbox offset is
       `t = floor((W − floor(11000·k + 0.5)) / 2)` (likewise for y).
   - noScale: `k = f32(1/20)` and the stage is centred: `t = (W − 550) / 2`.
2. **Device matrix of a spike** (`pix::RootMatrix`, `pix::Concat`, float32 concat 0x004c37e0).
   - The chain is: root `(4k, 0, 0, 4k, 4tx, 4ty)` → Level (camera, whole twips) → Ships10 → Shipik.
   - Entries are float32. Each child translation goes through the parent matrix and is rounded to a whole
     subpixel (`rint(f32(...))`, half to even) at every level.
3. **The probe** (`pix::Probe`). `hitTestPoint` gets the ball's control point as stage pixels from
   `localToGlobal`. It multiplies by 20 and truncates to twips. Then it maps the point to a **whole window
   pixel** `rint(f32(T·k)) + t` and multiplies by 4. Probes are therefore always multiples of 4 subpixels.
4. **Edges** (`pix::Xform`, shape 56 = the triangle (0,0)–(60,−193)–(120,0)).
   - Each vertex is mapped as `tx + rint(f32(a·f32(x) + c·f32(y)))`.
   - A straight edge is stored as a flat curve whose control point is the floored midpoint.
5. **Crossing test** (0x005c5bee, even–odd fill).
   - A ray is cast toward +x. A piece of curve that contains the probe in its box is halved (integer
     midpoints) while it is more than 4 subpixels across, up to depth 12.
   - A remaining piece counts as crossed whenever the probe is within its height range.
   - So slanted edges are fuzzy by up to one window pixel, leaning toward +x.
6. **Bounding box** (0x004b8450 → shape device bounds 0x004b4bd0).
   - Before the edges are looked at, the probe must lie in the shape's device box. The box is computed
     **with the current matrix** and tested inclusively.
   - For SWF > 8 the player then grows `xmax` and `ymax` by `(aa + 1) / 2` = **2 subpixels** (`pix::kBoundsPad`).
     Red Ball is SWF 10 and the Tournament Edition is SWF 9.
7. **Edge cache.**
   - The edge list is built when the shape is **redrawn**, and a redraw only happens after each frame's
     scripts.
   - `hitTestPoint` reuses that list. It does not rebuild it because the camera has moved since.
   - Only shapes inside the window are redrawn, so a spike that was off screen keeps older edges.
   - A shape that was never drawn gets its edges built by the hit test, with the current matrix.
   - rbsim keeps one "last drawn camera" per spike (`Sim::spDrawn*`, updated by `Sim::RenderSpikes` after
     every tick and once after a load or restart).

**Steps 6 and 7 together are the real-game spike glitch.** While the camera scrolls, a spike's hit area is
**its outline from the previous frame cut to its current box, padded on the right and bottom**. With the
camera moving left, that is often only a one-pixel sliver, or nothing. The same route therefore lives at one
window size and dies at another, even on plain, unturned spikes.

## 3. Evidence

All recordings were made with the team's projector (`SAFlashPlayer.exe` 11.4.402.287), run under Wine on
Linux with a virtual X display. A recording made on real Windows has not been compared yet.

**Still scenes (static sweeps, `rbsim calib`):**

| dump | rows | result |
|---|---|---|
| E8c, Level 3 plain spikes, 7 window sizes + 550×400 reference | 240,000 | 240,000 agree |
| E11 turned/scaled spikes, Levels 9, 10, 11, 15, 17 × 829×603, 935×925, 1280×720 | 1,172,010 | 1,172,010 agree |
| self-test 21: boundary rows of those sweeps (6 sizes, including a hidden size) | 58 | 58 |

**Moving camera.** The frame-scan mod (`docs/resolution/Shipik_framescan.as`, §5) scans spike edges on every
frame of a real-time playback. `tools/framescan` checks those scans against the model:

| playback | scanned rows | model (previous redraw) | no cache (current matrix) | redraw 2 frames back |
|---|---|---|---|---|
| Level 3 `d18e1d58`, 550×400 | 54 | **54** | 0 | 42 |
| same, 829×603 | 282 | **282** | 0 | 269 |
| same, 935×925 | 54 | **54** | 0 | 48 |

Without the 2-subpixel box padding, the model column drops to 46–48 of 54 rows (269 of 282 at 829×603). The
padding was found this way and then located in the player's code.

**Deaths (`rbsim verify`, every field of every frame, deaths on Flash's frame).**

- 28 real-time Flash playbacks are bit-exact up to and including the death frame:
  - 4 Level 3 routes;
  - showAll at 550×400, 640×480, 829×603, 935×925 and 1280×720;
  - noScale at 700×500 and 935×925.
- The death frame depends on the window. Self-test 22 keeps these Flash death frames:

| route | 550×400 | 640×480 | 829×603 | 935×925 | 1280×720 | noScale 700×500 | noScale 935×925 |
|---|---|---|---|---|---|---|---|
| `d18e1d58` | 62 | | 71 | 62 | | 62 | 62 |
| `e3n1a5n11n9d12…` (self-test 22) | 101 | 99 | **278** | 99 | 99 | 101 | 101 |

`docs/resolution/flash_runs.tar.xz` holds 17 of these logs: the stats logs, frame scans and `#ENV` lines.

## 4. Using it

- **CLI:** `--mathspikes 0 --window WxH` (showAll) or `--mathspikes 0 --window WxH --noscale`. This works on
  every command (`run`, `log`, `verify`, `calib`, `optimize`, `beam`, `bench`).
  - `verify` and `calib` take `mathspikes`, `window` and `scaleMode` from the log's `#ENV` line.
  - On the death row the logger writes input 0 (OutControl). `verify` replays that row with whichever input
    reproduces Flash's row: usually the previous tick's, but a route can change keys on exactly that frame.
- **Viewer:** the Spikes, Window and Scale boxes on the top row. Window takes a preset or a typed `WxH`.
  The settings are saved in `rbview.ini` under `[display]`.
  With the graphics marks on (Shift+H), each spike's hit area is drawn as the exact window pixels that kill this
  frame (`SpikeKillPixels`), next to the ball's control points (`BallControlPoints`); self-test 24 checks that a
  control point in a kill pixel is exactly a spike death (7,200 random cases, 6 window setups, plain and turned spikes).
- **Search:** routes found with `--mathspikes 0` are only valid for that window setup. Under `--mathspikes 0`,
  `beam` and `optimize` cost a little more per frame: each tick redraws the spike cache.

## 5. Recording a playback in Flash (how the evidence was made)

1. Use a Practice Hack SWF with the `#ENV` patch (`docs/MATHSPIKES_RESOLUTION.md` §4). For edge scans, also
   compile `docs/resolution/Shipik_framescan.as` in as `Shipik`:
   `ffdec -replace in.swf out.swf Shipik Shipik_framescan.as`.
   - It only acts under MATHSPIKES 0.
   - On every frame (EXIT_FRAME, before the redraw) it scans three rows of each spike near the ball with
     `hitTestPoint`.
   - The rows are appended to the `[` export as `#FS` lines.
2. Open the level, then type `/config mathspikes 0` (`/` opens the box). Set the window size and the scale mode
   (*View > Show All*).
3. Import the route: press `i`, **paste** it with Ctrl+V, then press Shift+1. This imports into state 1 and
   plays it in real time. Do not type the route. The game's key handler also sees digit keys and imports on the
   first one.
4. Press `[` **right after** the playback ends. The stats log is a ring buffer of about 1,200 frames.
5. Run `./rbsim verify rb1_stats.tsv`. For a scan build, also run `make framescan && tools/framescan rb1_stats.tsv`.

## 6. Not modelled / open

- **Quality** other than HIGH (a different anti-aliasing factor: probe grid, piece size and box padding all
  change), fullscreen, and the projector's zoom.
- **Off-screen spikes.** The culling rule (redraw only when the padded device box intersects the window) follows
  the static scans, where a spike row that was off screen at the last redraw answered with stale edges. No
  recorded death depends on it yet.
- **Real Windows.** Everything above was recorded under Wine. Wine's window and menu sizes differ from
  Windows', but the model only needs the Flash area that `#ENV` reports. One stats log recorded on Windows
  with mathspikes 0 would confirm the rest.
- **Tournament Edition specifics.**
  - Same Box2D and spikes as the Practice Hack with mathspikes 0.
  - The camera tween runs on Tweener's own ENTER_FRAME.
  - Level 15's money tween takes 0.3 s of real time instead of 9 frames. Not simulated yet.
