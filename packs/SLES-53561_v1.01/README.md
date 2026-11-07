# Game pack SLES-53561 v1.01 (PAL) - work in progress

A PS2RC game pack for the user's own PAL disc SLES-53561 (VER 1.01). Not affiliated with or endorsed by the game's
publisher or by Sony. The pack holds only our own code and facts about that disc version (addresses, sizes,
checksums); everything made from the disc lives in `../../work/` and never leaves this computer.

Status: **playable on the PSP (PPSSPP), ~12 fps at the school gate**, no sound output, movies skipped. See the
workspace README (`../../README.md`) for what is done, what is left and how to continue.

## What is here

| File | What |
|---|---|
| `pack.toml` | identity, memory map, 17 MWo3 overlays, translation / build settings |
| `hooks.def` | 116 Sony library functions + overlay loaders + movie skip hooks |
| `convert/discover.py`, `convert/common.py` | function discovery (main code + constructor code after .data) |
| `convert/trapset.py` | functions that must reach the hardware model (adapted from PS2RC's reference pack) |
| `src/hle_game.c` | overlay table, both overlay loaders, movie skip (MPEG.OLF), native table search 0x146970 |
| `src/hle_vu1.c`, `src/hle_vu1_native.h`, `src/hle_gfx_scan.c`, `src/psp/hle_ge_gs.c` | VIF1 / VU1 model, native mesh / skin / foliage programs, GS emulation and GE output |
| `convert/ide_scale.py` | stage transform: draw distances x0.75 (pack.toml `[[stage.transform]]`) |
| `src/hle_iop_servers.c` | disc stream server (CDSTREAM), silent responders for the audio and sound modules |
| `tools/sce_layout_match.py`, `tools/sce_align.py`, `tools/make_hooks.py` | naming the Sony library functions without a signature database |

## Build on the Mac (pspdev in ~/pspdev)

```
cd ~/BULLYPS2/PS2RC
export PS2RC_LOCAL=~/BULLYPS2/local.toml
python3 games/SLES-53561_v1.01/convert/discover.py
python3 ps2rc/pipeline/translate.py --pack SLES-53561_v1.01
python3 games/SLES-53561_v1.01/convert/trapset.py
SPLASH=0 bash mk/build.sh --pack SLES-53561_v1.01 psp 8
python3 ps2rc/pipeline/stage.py --pack SLES-53561_v1.01 --out ~/BULLYPS2/stage/PS2RC_SLES53561
```

Python 3.11+ with pyelftools is needed (`pip3 install pyelftools`). PS2RC's scripts are written for Linux/WSL;
on macOS some shell helpers (GNU `stat -c`, `flock`) may need the GNU coreutils / util-linux from Homebrew (Unknown:
not tried on macOS yet).

Credits: built with PS2RC (https://github.com/SandstoneDev/PS2RC) and universal-ps2-recompiler; parts adapted from
PS2RC's SLES-52541 pack under the PS2RC License.
