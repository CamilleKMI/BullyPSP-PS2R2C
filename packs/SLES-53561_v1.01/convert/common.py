"""Shared helpers of this pack's conversion steps (adapted from PS2RC's games/SLES-52541_v2.01/convert/common.py,
PS2RC License, https://github.com/SandstoneDev/PS2RC): the boot program and its code overlays as images.
Nothing here is written outside the work folder."""
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK_DIR = os.path.dirname(HERE)
ROOT = os.environ.get("PS2RC_ROOT") or os.path.abspath(os.path.join(PACK_DIR, "..", "..", "PS2RC"))
if not os.path.isdir(os.path.join(ROOT, "ps2rc")):
    ROOT = os.path.abspath(os.path.join(PACK_DIR, "..", ".."))
sys.path.insert(0, ROOT)
from ps2rc import config, pack as _pack  # noqa: E402
from ps2rc.image import Image, Segment, load_elf  # noqa: E402
from ps2rc.overlay import load_overlay  # noqa: E402
from ps2rc.analysis.functions import FunctionSet, discover_functions  # noqa: E402
from ps2rc.r5900 import decode  # noqa: E402

PACK_NAME = os.path.basename(PACK_DIR)
PACK = _pack.load(PACK_NAME)
WORK = config.work_dir()
DISC_DIR = config.disc_dir(PACK_NAME, required=False) or ""
BOOT_ELF = os.path.join(DISC_DIR, PACK.get("id.boot_elf")) if DISC_DIR else ""
OVERLAY_NAMES = list(PACK.get("overlays.files"))
CODE = tuple(PACK.get("memory.code"))
CTOR_CODE = tuple(PACK.get("memory.ctor_code"))
OVERLAY_WINDOW = tuple(PACK.get("overlays.window"))
FUNCTIONS = PACK.path("translate.functions")


def say(*a):
    print(*a, flush=True)


def main_image():
    """The stripped boot program and its code ranges: main code up to the VU1 microcode block, plus the
    static-constructor code after .data."""
    img = load_elf(BOOT_ELF).stripped()
    return img, [CODE, CTOR_CODE]


def overlay_image(img, name):
    ov = load_overlay(os.path.join(DISC_DIR, name))
    mem = ov.memory()
    seg = Segment(vaddr=ov.load_addr, filesz=len(ov.raw), memsz=len(mem), flags=7, data=mem)
    oimg = Image(path=ov.path, entry=ov.text_addr, gp=img.gp, segments=list(img.segments) + [seg], overlay_slots=[])
    return ov, oimg, [(ov.text_addr, ov.text_addr + ov.text_size)]


def discover_overlays(img):
    out = {}
    for name in OVERLAY_NAMES:
        _ov, oimg, code = overlay_image(img, name)
        out[name] = (oimg, code, discover_functions(oimg, code))
    return out


def load_functions(path=FUNCTIONS):
    with open(path, "rb") as f:
        return pickle.load(f)
