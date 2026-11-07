"""dis.py ELF START END - disassemble the boot program (stripped) with PS2RC's decoder (game-derived output: local only)."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "PS2RC"))
from ps2rc.image import load_elf
from ps2rc.r5900 import decode
img = load_elf(sys.argv[1])
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
for x in range(a, b, 4):
    i = decode(img.word(x), x)
    print("%08x  %08x  %s" % (x, img.word(x), i.text))
