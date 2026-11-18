#!/usr/bin/env python3
"""PPM -> PNG for the host UI renders; also a contact sheet.   ui_png.py DIR"""
import sys, glob
from PIL import Image
d = sys.argv[1]
files = sorted(glob.glob(d + "/*.ppm"))
for f in files:
    Image.open(f).save(f[:-4] + ".png")
print(len(files), "images")
