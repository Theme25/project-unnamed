# Red Ball 1 — per-frame stats logging mod (verification data for `rbsim`)

This document is self-contained. It explains **what** to log from the real game,
**how** to add the logging to the SWF, and **what to hand back** so the C++
simulator (`rbsim`, repo `Theme25/project-unnamed`) can be checked bit-for-bit
against Flash.

---

## 1. Background (for a fresh chat)

- `rbsim` is a C++ port of the Box2DFlash 2.0.x engine embedded in *Red Ball 1*,
  plus the game's control logic. It must match Flash **bit-exactly** so that
  routes found by brute force replay identically in the real game.
- Target SWF: `Red_Ball_-_Practice_Hack.swf` (AS3, SWF v10). It already has TAS
  playback, savestates (keys 0–9), RLE input strings and `FileReference` export.
  All names below come from its decompiled source (JPEXS FFDec).
- Verification works by comparing **per-frame player state** between the game
  and `rbsim` for the same input string. The first frame and field that differ
  locate the bug.

## 2. Ground rules for the mod

1. **Logging must not change the simulation.**
   - Only *read* fields.
   - Do **not** call `m_world.Query`, `GetBodyAtPoint`, `Step`, `SetXForm`,
     `WakeUp`, or anything else on the world or bodies. Queries mutate
     broadphase state.
   - Reading `GetPosition()` and `GetLinearVelocity()` is fine: they return
     references without modifying anything.
2. **Edit only `Game` and `Levels.Level`.** Never recompile any `Box2D.*`
   class. The decompiled Box2D source is known to be lossy: JPEXS silently
   dropped two statements from `b2Collision.FindMaxSeparation`, and
   recompiling it would break physics.
3. **The same caution applies to `Game` and `Level`.** Recompiling a class
   from decompiled source can also drop statements there. Keep the edits
   small, and run the sanity check in §8 before trusting any log.
4. **Doubles must be logged as exact hex**, not only as decimal strings (see §5).

## 3. What to log

### 3.1 Per frame — one line immediately after every `m_currLevel.Update(...)` call

| # | Field | AS3 expression (inside `Game`) | Why |
|---|---|---|---|
| 1 | `tick` | own counter, +1 per `Update` call (ignore `isTimeStop`) | alignment |
| 2 | `frame` | `this.frameCount` (after its increment) | matches game timer |
| 3 | `in` | `4*int(L.Left) + 2*int(L.Up) + int(L.Right)` | input actually used |
| 4 | `px`, `py` | `b.m_xf.position.x`, `.y` | position (m) |
| 5 | `vx`, `vy` | `b.m_linearVelocity.x`, `.y` | **after** input nudges |
| 6 | `a` | `b.m_sweep.a` | angle (rad) |
| 7 | `w` | `b.m_angularVelocity` | spin |
| 8 | `sleepT` | `b.m_sleepTime` | sleep timer |
| 9 | `flags` | `b.m_flags` (uint) | sleep bit = 8, frozen = 2 |
| 10 | `pC`,`pL`,`pR` | ground probes (see §4.2) | grounded logic |
| 11 | `nCB` | `L.getmyContactListener().playerContactBodies.length` | jump gate |
| 12 | `cb` | names of the entries in `playerContactBodies` (see below) | listener-bug check |
| 13 | `nC` | `L.m_world.m_contactCount` (use a getter if `m_world` is not public) | contact lifecycle |
| 14 | `sx`,`sy`,`sr` | `L.getplayerBox().x`, `.y`, `.rotation` | twip rounding |
| 15 | `ts` | `L.isTimeStop` | win detection |

Here `L = this.m_currLevel` and `b = L.getplayerBox().body`.

For `cb`, write one name per entry of `playerContactBodies`: `e.m_userData ? e.m_userData.name : "ground"`, joined with `,`.

Fields 4–9 and 14 must be written as **hex** (§5). A decimal copy alongside is optional and only for readability.

### 3.2 Event lines

- `R` — whenever `SetLevel(m_currId, true)` runs (an input code 8 restart).
- `LEVEL <id> <lastCheckNum>` — after every `SetLevel`. Follow it with one
  frame-0 state line (`tick = 0`) logged **before any `Update`**; this verifies
  level construction.

### 3.3 One-time calibration dumps (hotkey, independent of gameplay)

These settle the three open assumptions in `rbsim` directly.

**A. Math.sin / Math.cos / Math.atan2.**
- For `k = 0 … 4095` let `x = (k - 2048) * 0.0078125 + k * 1e-7`.
  Log `hex(x) hex(Math.sin(x)) hex(Math.cos(x))`.
- Also log `hex(Math.atan2(Math.sin(x), Math.cos(x)))`.

**B. Twip quantisation of `DisplayObject.x`.**
- Use a throwaway `Sprite` that is **not** on the stage.
- For `k = 0 … 1199` set `s.x = (k - 600) * 0.0137 + 0.001`, read it back,
  and log `hex(written) hex(s.x)`.
- Repeat for `s.y`.

**C. Rotation round-trip.**
- For `k = 0 … 719` set `s.rotation = (k - 360) * 0.73`, read it back, and
  log both in hex.

**D. Environment.**
- Log `Capabilities.version`, `Capabilities.os`, and
  `Capabilities.cpuArchitecture` (wrap in `try/catch`; it may not exist on
  older players).
- Log the SWF name you modded.

Note that `sx`, `sy`, `sr` in the per-frame lines also expose twip behaviour on real values.

## 4. Where to hook in

### 4.1 `Game.as` — both update paths

The game advances the level in **two** places, and both need the logger.

**Real-time or playback path — `UpdateHandler`:**
```as3
// inside while(this.gameTick >= 1)
if(this.isPlayback) {
   _loc3_ = int(saveState[0].shift());
   if(_loc3_ == 8) {
      this.SetLevel(this.m_currId,true);
      logEvent("R");                        // <-- add
      this.gameState.push(8);
      _loc3_ = int(saveState[0].shift());
   }
   ...
}
...
this.m_currLevel.Update(param1);
if(!this.m_currLevel.isTimeStop) { ++this.frameCount; COMM.broadcast("updateOffset"); }
logFrame();                                  // <-- add (after the frameCount increment)
```

**Instant load-state path — `LoadState`**, the `else` branch that loops over `saveState[saveNum]`:
```as3
if(saveState[saveNum][index] == 8) {
   this.SetLevel(this.m_currId,true);
   logEvent("R");                            // <-- add
} else {
   ... Left/Up/Right ...
   this.m_currLevel.Update(new Event(Event.ENTER_FRAME));
   if(!this.m_currLevel.isTimeStop) ++this.frameCount;
   logFrame();                               // <-- add
}
```

This instant path is the most convenient way to generate logs: load a state
and the whole run is simulated immediately.

**Level header.** At the end of `SetLevel(...)`, add:
```as3
logEvent("LEVEL " + m_currId + " " + Level.lastCheckNum);
logFrame(0);  // frame-0 state line (tick = 0)
```

### 4.2 `Levels/Level.as` — expose the ground probes

The probes are locals in `Level.Update(param1:Event)`:
```as3
var _loc4_:Boolean = Boolean(this.GetBodyAtPoint(...GetPosition().y + 0.4, true));
var _loc5_:Boolean = Boolean(this.GetBodyAtPoint(...GetPosition().x - 0.2, ... + 0.38, true));
var _loc6_:Boolean = Boolean(this.GetBodyAtPoint(...GetPosition().x + 0.2, ... + 0.38, true));
```
1. Add `public var dbgPC:Boolean, dbgPL:Boolean, dbgPR:Boolean;` to the class.
2. Right after those three lines, add `dbgPC = _loc4_; dbgPL = _loc5_; dbgPR = _loc6_;`.
3. Do **not** call `GetBodyAtPoint` again from the logger.

`m_world` is `protected` in `Level`, so `Game` can't reach it directly. Also add
`public function dbgContactCount():int { return m_world.m_contactCount; }`
(required; `logFrame` calls it).

## 5. Encoding helpers (AS3)

```as3
import flash.utils.ByteArray;
import flash.system.Capabilities;
import flash.net.FileReference;

private var dbgLog:Array = [];
private var dbgTick:int = 0;
private static var dbgBA:ByteArray = new ByteArray();   // BIG_ENDIAN by default

private static function hex(n:Number):String {
   dbgBA.position = 0; dbgBA.writeDouble(n); dbgBA.position = 0;
   var s:String = "";
   for (var i:int = 0; i < 8; i++) {
      var b:uint = dbgBA.readUnsignedByte();
      s += (b < 16 ? "0" : "") + b.toString(16);
   }
   return s;                                 // == printf("%016llx", bits)
}

private function logEvent(s:String):void { dbgLog.push(s); if (s.indexOf("LEVEL") == 0) dbgTick = 0; }

private function logFrame(forceTick:int = -1):void {
   var L:* = this.m_currLevel;
   var b:* = L.getplayerBox().body;
   var cbl:Array = L.getmyContactListener().playerContactBodies;
   var names:Array = [];
   for each (var e:* in cbl) names.push(e.m_userData ? e.m_userData.name : "ground");
   var t:int = forceTick >= 0 ? forceTick : ++dbgTick;
   dbgLog.push([t, this.frameCount,
      4*int(L.Left) + 2*int(L.Up) + int(L.Right),
      hex(b.m_xf.position.x), hex(b.m_xf.position.y),
      hex(b.m_linearVelocity.x), hex(b.m_linearVelocity.y),
      hex(b.m_sweep.a), hex(b.m_angularVelocity),
      hex(b.m_sleepTime), b.m_flags,
      int(L.dbgPC), int(L.dbgPL), int(L.dbgPR),
      cbl.length, names.join(","), L.dbgContactCount(),
      hex(L.getplayerBox().x), hex(L.getplayerBox().y), hex(L.getplayerBox().rotation),
      int(L.isTimeStop)].join("\t"));
}

// Call from the keyboard handler: FileReference.save() only works inside a user-input event.
private function dbgExport():void {
   var hdr:String = "#tick\tframe\tin\tpx\tpy\tvx\tvy\ta\tw\tsleepT\tflags\tpC\tpL\tpR\tnCB\tcb\tnC\tsx\tsy\tsr\tts\n";
   new FileReference().save(hdr + dbgLog.join("\n"), "rb1_stats.tsv");
   dbgLog = [];
}
```

**Hotkeys.** Add two unused keys in the existing `KeyboardEvent` handler: one
calls `dbgExport()`, and one runs the §3.3 calibration dumps and saves them as
`rb1_calib.tsv`. The game already uses `FileReference` for exporting strings,
so this pattern is proven to work in this build.

## 6. Test runs to record (Level 1, then your real TAS)

Start each run from a **fresh Level 1** with `lastCheckNum = 0`, played back
through the instant `LoadState` path. Export one file per run.

| name | RLE input | exercises |
|---|---|---|
| T1 idle | `n90` | falling onto the platform, resting contact, falling asleep |
| T2 roll right | `n30d60n60` | ground acceleration and cap, rolling off the edge into the gap, death line |
| T3 roll left | `n30a40n60` | left side, falling off the left edge |
| T4 jump + air | `n20w1d25n40` | one-frame jump, airborne +0.25 steps, landing |
| T5 held jumps | `n20q15e15w10n30` | repeated jumps, air control both ways |
| T6 L+R | `n20S30W10n30` | same-frame Left+Right (live velocity reads) |
| T7 restart | `n20d20R1d20n20` | `R` handling, re-construction |
| T8 real TAS | your Level 1 TAS string | barrier, exitPlatform landing, win / `isTimeStop` |
| T9+ | later levels' TAS strings | only once joints and level extraction land in `rbsim` |

Also export the calibration dump (§3.3) once.

## 7. What to hand back

- `rb1_stats_T1.tsv` … `rb1_stats_T8.tsv`: tab-separated, one header line,
  then `LEVEL`/`R` event lines and frame lines.
- `rb1_calib.tsv`: the sin/cos/atan2, twip and rotation tables, plus the
  environment line.
- The exact input string used for each run. The mod can also dump
  `EncodeOutRLE(gameState)` into the file header.

`rbsim` will replay each string with `./rbsim run --inputs "<rle>" --hex` and
report the first mismatching tick and field.

## 8. Sanity checks before trusting the logs

1. **Unmodified behaviour.** Play a known TAS in the original SWF and in the
   modded SWF. The final timer value must be identical, and so must the frame
   of the finish.
2. **Frame 0.** The `LEVEL` line's frame-0 values must equal the checkpoint:
   `px = 16.9/30`, `py = 249.3/30` for Level 1, and velocities must be `0`.
3. **Hex sanity.** `hex(1.0)` must return `3ff0000000000000`.
4. **Both paths agree.** A run replayed in real time and the same run via
   instant load-state must produce identical logs.
