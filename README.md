# Bully / Canis Canem Edit (PS2, SLES-53561) → PSP — PS2RC port

Based on PS2RC by SandstoneDev
https://github.com/SandstoneDev/PS2RC
Community: https://discord.gg/wV8x46ZTDu

Experimental port, not affiliated with or endorsed by Rockstar Games, Take-Two, Sony or SandstoneDev.
**This repository contains no game data**: no disc files, no translated game code, no EBOOT. Every user starts
from **their own copy** of the PAL disc SLES-53561 (VER 1.01). Translation and compilation happen on their own
machine, for personal use. See `PS2RC/LICENSE` (PS2RC licence 1.2) and `PS2RC/NOTICE.md`.

## Where the port stands

The game boots on PSP (tested in PPSSPP) and reaches gameplay: title screen, loading, Jimmy in front of the
school, movement, HUD, text.

The reference figure is measured in headless PPSSPP (PSP at 333 MHz), in game in front of the school, with the
draw distance at 0.75:

| Point in time | ms per frame | fps |
|---|---|---|
| Start of the speed work | ~680 | ~1.5 |
| Session 12 | 116 | 8.6 |
| Session 13 (current) | **80-81** | **~12.4** |

The PS2 game targets 30 fps, so game time runs slower than real time: the game is slowed down, it does not drop
frames.

| Area | State |
|---|---|
| Game code (EE) | translated to C by PS2RC (ABI mode, registers as locals, VU0 macro mode in locals) |
| VU1 graphics | microprograms rewritten natively (meshes, skinning, foliage) and sent straight to the PSP GE; the others go through a generated fast translation (`tools/vu1_fast.py`) |
| GS | lightweight emulation, with a texture cache and deferred uploads written block by block |
| Draw distance | ×0.75 on near objects, applied by the converter (`[[stage.transform]]`) |
| Sound | the game's modules (SCREAM, AUDVAG) run, but on a **silent** SPU: no audio output |
| Movies | skipped |
| Full-screen passes (blur, glow) | skipped on PSP (would need render-to-texture on the GE) |
| Saves | memory card emulated in `memcard/` (not tested in depth) |

## What is left to do

1. **Speed: ~14 ms short of 15 fps (~66 ms per frame).** There is no single big item left. Measured breakdown
   (share of busy CPU, PPSSPP):
   - translated game code: ~30 %, flat profile (the largest function is ~2 %);
   - skinning (`skin_batch`): ~9 %, almost all in the per-vertex VFPU loop;
   - VIF command parsing (`vif1_chunk_`): ~7 %;
   - mesh batch setup and submission (`nat_batch`, `mc_draw`, `nat_emit`, `set_3d_state`…): ~15 % for ~1,100 batches per frame;
   - GS packets (`gs_packets_`): ~4 %.

   Already tried without gain (do not redo blindly):
   - a native replacement for the sphere/box test at `0x1313D0`: slower, because the caller has to save its registers around the call;
   - a per-call-site cache for indirect calls (`PS2RC_IC=1`): slower;
   - float saturation handled in the FPU (`fresf_`): slower under PPSSPP;
   - reusing skinned vertices from one frame to the next: only 4 batches out of 257 keep the same bones.

   Open leads:
   - replay DMA blocks that are identical from one frame to the next (~400 of 1,120), estimated gain 2-4 ms;
   - natively replace more game functions that make indirect calls, like the binary search at `0x146970` (already done, −1.6 ms);
   - **measure on a real PSP**: PPSSPP emulates the VFPU with an overhead, so real figures may be better.
2. **Sound**: hook a real SPU2 model (ADPCM voices and mixing) to the PSP audio output. Estimated CPU cost
   5-15 %, to be put behind a setting.
3. **Movies**: convert the PSS videos to PMF with the converter (`--movies convert`) and play them with
   `sceMpeg`.
4. **Full-screen passes**: depth mask and glow, using render-to-texture on the GE.
5. **Real PSP**: check memory (PSP-2000/3000 with extended memory, `mem_volatile=1`) and stability over a long
   play session.

## Layout

```
README.md                this file
PS2RC/                   the PS2RC v0.2.1 engine (commit 8cd56bb), WITH this port's changes
packs/SLES-53561_v1.01/  the game pack: hooks, native code (src/), IOP modules, conversion scripts
tools/                   working tools (PSP profiling, VU1 lab, generators)
env.sh                   workspace environment variables
local.toml.example       machine settings template (copy to local.toml)
work/                    (not in the repository) everything made from YOUR disc: private
```

Files changed in PS2RC compared with v0.2.1 (`git -C PS2RC diff` against a clean copy):
- `ps2rc/backend_c.py` and `ps2rc/vu/macro.py`: extra outputs (XOUT), functions that do not touch VU0 (VFREE),
  DI/MFC0 as locals, VU0 macro mode and MMI in local variables;
- `runtime/r5900_rt.h` and `hle/core/*`: `rt_di`, `rt_mfc0`, `vu0_clipw_l`, `RC_MAX_OVERLAYS` raised to 32;
- `ps2rc/convert/{recipe,stage,pipeline}.py`: `[[stage.transform]]`, which lets the pack rewrite a disc file at
  conversion time, keeping its size;
- `ps2rc/pipeline/{translate,parallel}.py`, `ps2rc/analysis/abi.py`, `ps2rc/iop/rt/iop_rt_int.h`.

## Continuing the work

You need:
- Python 3.11+ with `pip install pyelftools pillow pytest`;
- the pspdev toolchain (`~/pspdev`);
- PPSSPP, preferably built headless (`PPSSPPHeadless`) for measurements.

```sh
cp local.toml.example local.toml          # then put your paths in it (work, extracted disc)
ln -s ../../packs/SLES-53561_v1.01 PS2RC/games/SLES-53561_v1.01
. ./env.sh
```

**A. Just play / produce the PSP folder** (the converter does everything):
```sh
cd PS2RC && python3 -m ps2rc.convert "/path/to/your_bully.iso" --out ~/output
# -> ~/output/PS2RC_SLES53561 to copy into PSP/GAME/ (≈3.8 GB on a Memory Stick)
```

**B. Develop**: you need a `work/` filled from your disc. The disc is extracted to `work/disc`, with
`work/discmap.txt` and `work/iso_meta.bin` (the converter, or the conversion above with `--work`). Then:
```sh
cd PS2RC
python3 games/SLES-53561_v1.01/convert/discover.py
python3 games/SLES-53561_v1.01/convert/non_abi.py
python3 games/SLES-53561_v1.01/convert/exact_extra.py
python3 games/SLES-53561_v1.01/convert/extra_outputs.py
python3 ps2rc/pipeline/translate.py --pack SLES-53561_v1.01
python3 games/SLES-53561_v1.01/convert/trapset.py
bash mk/build.sh --pack SLES-53561_v1.01 host 4                 # PC build (frame comparison)
SPLASH=0 bash mk/build.sh --pack SLES-53561_v1.01 psp 4         # -> work/psp/EBOOT.PBP
python3 ps2rc/pipeline/stage.py --pack SLES-53561_v1.01 --out <folder>/PS2RC_SLES53561
```

**Private files to regenerate** (they are derived from the game, so never in the repository). The build works
without them, only slower:
- `work/vu1aot/hle_vu1_fast.h`: VU1 microprograms in fast translation. Run a PC build with
  `HLE_VU1_PROF=/absolute/path` to collect `prog_*.code` and the histograms, then
  `FAST_CLAMP=1 python3 tools/vu1_fast.py prog_000_c3651f62.code PCHIST work/vu1aot/hle_vu1_fast.h`.
- `work/vu1aot/hle_vu1_aot.h`: `tools/vu1_aot.py` (same principle).
- `work/iopgen/`: translated sound modules (`convert/iopgen.py`).

**Measuring**: in the test folder's `ps2rc.cfg`, set `max_seconds=330`, `shot_frame=1250` and `zprof=1` (zone
profile). The `PERF` lines in `session.log` give ms per frame. Run only one PPSSPP instance at a time: its clock
follows the machine's load. For a per-address profile, use the `PPSSPP_PCPROF` / `PPSSPP_PCPROF_FROM`
environment variables, then `tools/psp_pcprof_report.py`, `psp_pcprof_lines.py` and `psp_pcprof_callers.py`.

Useful `ps2rc.cfg` switches:
- `x_folnat=0`: foliage through the VU program;
- `x_cellput=0`: textures written pixel by pixel;
- `x_meshcache=0`: no mesh cache;
- `x_vu1fast=0`: no fast translation.
