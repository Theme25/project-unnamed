# The 64-bit Flash Players: `--player activex | x64 | x64-nofma`

The simulator's reference is the 32-bit Flash Player 11.4.402.287. The 32-bit players 30.0.0.113 and 32.0.0.465
were checked against it and are bit-identical for everything the simulator models: Math natives, fixed-point trig,
float32 matrices, hit tests. Use any of them for TAS work.

The 64-bit builds are different programs. Their C runtime has no x87, so `Math.sin`/`Math.cos` come from other
libm code. Several display routines were also recompiled with SSE single precision or rewritten. A route recorded
on a 32-bit player can therefore play out differently on a 64-bit one. Box2D rebuilds every body's rotation from
`Math.sin`/`Math.cos` each step, so one last-bit difference is enough to send a route somewhere else.

These modes reproduce them:

| `--player` | player | its `Math.sin`/`cos` |
|---|---|---|
| `11.4` (default) | every 32-bit player (11.4, 30, 32) | Intel SSE2 libm (`src/libm_intel.S`) |
| `activex` | the "ActiveX Flash Player" (`ActiveX_Flash_Player.exe`, an x64 .NET host) with `Flash64_11_5_502_149.ocx` | VS2008 x64 CRT (AMD libm) |
| `x64` | Flash Player 30.0.0.134 x64 projector (`Flash_Player34bit.exe`) on a CPU with FMA3 + AVX | UCRT, FMA3 path |
| `x64-nofma` | the same projector on a CPU without FMA3 (before about 2013) | UCRT, SSE2 path |

The UCRT picks its path at start-up: cpuid FMA3 + OSXSAVE + AVX and XCR0 YMM state. `x64` executes FMA
instructions, so it runs only on such a CPU; the simulator refuses it otherwise. `x64-nofma` gives the same results
as `activex` on almost every input.

The ActiveX host loads whichever 64-bit OCX is registered on the PC. `activex` is the 11.5.502.149 OCX. A newer x64
OCX (VS2015+) would be expected to behave like `x64`, but that has not been checked.

**Where to set it:**
- **rbsim:** `--player P` on any command.
- **rbview:** Options tab, "Flash Player", saved in `rbview.ini`. A saved route records a non-default player in an
  `#ENV player` line.
- **Smartforce:** Advanced settings, "Flash Player". "Use current route" copies the viewer's choice.

## What differs, and how it is reproduced

| routine | 32-bit players | 64-bit players (both) | in the simulator |
|---|---|---|---|
| `Math.sin`, `Math.cos` | Intel SSE2 libm | x64 CRT, three variants (table above) | the players' own routines, re-assembled (`src/libm_x64_ocx.S`, `src/libm_x64_fp30.S`) |
| 16.16 divide (inside the timeline-rotation atan2) | magnitude rounded half up, sign applied after | `q = trunc((a << (bits+1)) / b)`, then `floor(q/2)` plus the low bit of `q`, away from zero: negative results are often one lower | `FlashFixDivX64` |
| float32 point / rect transform | x87: exact products, one rounding to float | SSE: each product rounded to float, then the sum | `FlashXformX/Y`, `pix::Xform` |
| float32 concat | same as above | same as above | `pix::Concat` |
| float32 inverse | double intermediates | all float | `FlashInvertX64` |
| `Math.pow(2, -10/31)` (camera tween) | `0x3fe996a2ea68dd55` | the same in both | unchanged |
| fixed-point sin/cos (rotation setter), x/y setter (×20, truncate), SWF≥8 float-matrix gate, hitTestPoint curve test | — | identical | unchanged |

The 64-bit display rules are the same in the OCX and in the 30 x64 projector. Native comparison over 2,000,000
random float matrices found 0 differences in the point transform, concat and inverse. The fixed-point
sin/cos/atan2/divide matched on 42,791 / 60,000 / 60,000 inputs.

**Effect on Red Ball:**
- **Physics:** diverges as soon as a `Math.sin`/`cos` result differs. About 3% of calls differ by 1 ulp from the
  32-bit players; `x64` differs from `activex` on about 1% more.
- **Timeline-placed clips:** about 16% of realistic 16.16 matrices read back a `rotation` up to 58/65536° different.
  Spike and goal geometry built from those rotations changes accordingly.
- **Hit tests:** `hitTestObject` / `getBounds` boxes and `--mathspikes 0` pixel tests come out one twip or subpixel
  different on about 1% of transformed points.

## How it was verified

**`Math.sin`/`cos`.** `tools/pe2gas.py` re-emits the players' own instructions as GNU assembly:
- jumps become labels;
- RIP-relative data is copied into `.rdata` chunks;
- the CRT's NaN/Inf error reporters become inline code (they never change a finite value).

The extracted routines were run natively next to the original code mapped at its image base, with identical
results on 100,000,000 inputs per routine and per path:
- `[-π, π]`, `[-100, 100]`, `[-10⁴, 10⁴]`, `[-10⁶, 10⁶]` and `[-10⁹, 10⁹]`, which covers the Payne–Hanek
  reduction for large angles;
- tiny values down to the subnormals;
- random finite bit patterns.

**Display routines.** The C++ ports were checked against the players' machine code run natively:
- 3,000,000 cases each (1,000,000 per player mode) for atan2fx, point transform, `pix::Xform`, concat and inverse;
- the BallBounds closed form against the rect transform of the PlayerBox.

All gave 0 differences.

**`rbsim test` §26** holds spot values produced by the real routines in every mode, so a broken build shows up:
- `Math.sin`/`cos` at 10 angles × 4 modes, chosen where the modes disagree;
- 6 atan2 values;
- 7 matrix results.

The Windows build gives the same results as Linux in every mode. Checked under Wine 9: full test suite and
replays.

**Not verified:**
- **Full replay against the real 64-bit players.** No 64-bit Flash logs exist yet. Record one with the logging
  mod in the 64-bit player and run `rbsim --player x64 verify log.tsv`.
- **The 64-bit players' window/letterbox arithmetic for `--mathspikes 0`.** It is assumed equal to 11.4: a float32
  of one division is identical either way, but the letterbox rounding was not located in the x64 code.

## Regenerating the assembly

Image bases: OCX 0x30000000, 30 x64 projector 0x140000000.

```sh
python3 tools/pe2gas.py Flash64_11_5_502_149.ocx rb_x64ocx sin=0x313ef490:export cos=0x313ef000:export \
    rem=0x3088a8f4 stub=nan@0x3088af80 stub=nan@0x3088afa8 > src/libm_x64_ocx.S
python3 tools/pe2gas.py Flash_Player34bit.exe rb_x64f30 sin=0x14058ff20:export cos=0x140590490:export \
    rem=0x140599b50 remfma=0x140599ea0 redfma=0x14059a090 stub=ucrtnan@0x14059a130 stub=ucrtnan@0x14059a108 \
    flag=rb_x64f30_use_fma3@0x141025c44 > src/libm_x64_fp30.S
```

**Other addresses:**

| routine | OCX | 30 x64 |
|---|---|---|
| core / playerglobal native tables (24-byte `{id, thunk, handler}`) | 0x30f6f160 / 0x30e9fde0 | 0x140db8330 / – |
| UCRT FMA3 switch | – | 0x141025c44 |
| 0.25° sine table | 0x30e53450 | 0x140c92d00 |
| fixed sin, cos, sine core | 0x300726f0, 0x300727a0, 0x30072600 | 0x140368170, 0x140368030, 0x1403683b0 |
| atan2fx, its helper, atan core, 16.16 divide | 0x30072ac0, 0x30072a00, 0x30072870, 0x30072580 | 0x140367eb0, 0x140367f80, 0x140368220, 0x1403680f8 |
| point, delta, rect transform | 0x30078b70, 0x30078cc0, 0x30078e00 | 0x1402b4c90, –, – |
| concat, inverse | 0x30079350, 0x300796d0 | 0x1402b3230, 0x1402b3e90 |
| hitTestPoint curve test | 0x300427bc | – |
| `Math.pow` (avmplus) | 0x306c27c0 | 0x140992ca0 |

A dash means not located: those routines were not needed in that binary.
