# Building Mini-Grain

The build makes, in `build/`:

| File | What |
| --- | --- |
| `grain.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `mini-grain.fwsc` | the installable package (app + loader) |

## Prerequisites (Linux x86-64; on macOS the JieLi tools run in Docker, see `tools/build.py`)

- Python 3 with Pillow and fontTools (`pip3 install Pillow fonttools`): the screen's fonts are rasterised at build time.
- The JieLi toolchain (clang 4.0.1 for pi32v2): `tools/get_toolchain.sh` installs it to `~/.jieli/toolchain`.
- The JieLi AC79 SDK (Apache-2.0); the package uses three of its files (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`,
  `cfg/eq_cfg_hw.bin`):

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

## Build and install

```
./build.sh                       # JIELI_TOOLCHAIN and AC79_SDK override the default places
./flash.sh                       # the update goes over USB (tools/fm1_install.py): follow its prompts
```

`./build.sh --release 0.1` makes a release build (`build/mini-grain-0.1.fwsc` and `build/release-0.1/`). To go
back to the stock firmware, install the maker's package the same way.

Options (environment, `0` or `1`; defaults in `firmware/src/grain.c`): `FELUCCA_UAC` (USB audio out, default 1),
`FELUCCA_UAC_48K` (offer 48 kHz too, 1), `FELUCCA_UART` (TRS MIDI in, 1), `FELUCCA_OTA` (the update entry, 1).

The build checks that the image fits, that no code calls into the chip's ROM (the compiler has no FPU: every float
operation would be a library call, so the whole engine is fixed point), and that registers are touched only in
`firmware/hal/`.

## Host tests

```
tests/run_tests.sh               # needs cc, python3 with numpy and Pillow
```

The engine, the effects, the sequencer, the sample link (on a simulated flash) and the screens run on the computer; the
screens are written as PNGs in `build/ui/`. If Node and Chrome are installed, the sample page (`web/index.html`) is also
run in headless Chrome against the firmware's own link code.
