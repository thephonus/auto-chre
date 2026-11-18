#!/bin/sh
# auto-chre's tests on the computer: the engine, the sequencer, the mutations, the effects and the screens.
# Needs cc, python3 and Pillow. Writes build/demo.wav (70 s of the machine) and build/ui/*.png (every screen).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/gen build/ui
python3 tools/gen_ac.py build/gen/ac_gen.h
python3 tools/gen_aa_font.py build/gen/ui_fonts.h --preset inter-tight > /dev/null
python3 tools/gen_aa_keycaps.py build/gen/ui_keycaps.h > /dev/null
python3 tools/gen_ui_palettes.py build/gen/ui_palettes.h > /dev/null
CC="cc -O1 -Wall -Wno-unused-function -Wno-unused-variable -fno-tree-loop-distribute-patterns -Ifirmware/src -Ibuild/gen"
$CC tests/host_ac.c -o build/host_ac -lm
echo "== auto-chre: voices, sequencer, mutations, effects, screens"
build/host_ac build/demo.wav build/ui
python3 tests/ui_png.py build/ui
echo "SELFTEST PASS"
