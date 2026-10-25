# Building MASIF

## What you need

- Linux x86-64 (natively), or macOS / Windows with Docker: the JieLi toolchain is Linux x86-64 only, and the
  build runs each tool in a `linux/amd64` container where it cannot run natively.
- Python 3 with Pillow: `python3 -m pip install Pillow`
- The JieLi pi32v2 toolchain (clang 4.0.1): `tools/get_toolchain.sh` installs it to `~/.jieli/toolchain`.
- The JieLi AC79 SDK (Apache-2.0). The package takes three files from it (`uboot.boot`, `cfg_tool.bin`,
  `eq_cfg_hw.bin`); they are not part of this tree. The build checks their hashes against this tag:

      git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
          https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK

- Node.js (optional: the web tests).

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations. `tools/masif_preflight.sh` checks that everything
is in place without building.

## Build

    MASIF_ENABLE_TRACKER=1 MASIF_KIT=none MASIF_UI=1 sh ./build.sh               # -> build/masif.fwsc (FM-1_960)
    MASIF_ENABLE_TRACKER=1 MASIF_KIT=none MASIF_UI=1 sh ./build.sh --release 0.1 # -> build/masif-0.1.fwsc (FM-1_961)

Read the output for `FAIL` or `build: checks failed`: the package is then not written. The checks include no
64-bit division (the chip has no libgcc), no constant in the vendor's 0xFFC00000-0xFFD00000 range, and RAM within
96 KB.

## Host tests

The tracker, player, screens, sampler, song store and the link run on the computer too. From the repository root:

    mkdir -p build/host/ui
    cc -std=gnu99 -O1 -Wall -Wextra -Ifirmware/src -o build/host/test_tracker host-tests/test_tracker.c firmware/src/masif_tracker.c firmware/src/masif_runtime.c && build/host/test_tracker
    cc -std=gnu99 -O1 -Wall -Wextra -Ifirmware/src -o build/host/test_runtime host-tests/test_runtime.c firmware/src/masif_tracker.c firmware/src/masif_runtime.c && build/host/test_runtime
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=signed-integer-overflow -fno-sanitize-recover=all -o build/host/voice_limits host-tests/test_voice_limits.c && build/host/voice_limits
    cc -std=gnu99 -O2 -Wall -Wextra -Wno-unused-function -o build/host/masif_integration host-tests/test_integration.c && build/host/masif_integration
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,signed-integer-overflow -fno-sanitize-recover=all -Ifirmware/src -Ifirmware/gen -Ibuild/gen -o build/host/test_ui_msf host-tests/test_ui_msf.c && build/host/test_ui_msf build/host/ui
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/src -Ifirmware/gen -Ibuild/gen -o build/host/test_sampler host-tests/test_sampler.c -lm && build/host/test_sampler && node tests/masif_pool_dir_test.mjs
    cc -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/src -o build/host/test_store host-tests/test_store.c firmware/src/masif_tracker.c firmware/src/masif_runtime.c firmware/src/masif_songfile.c firmware/src/masif_store.c firmware/src/masif_demo.c && build/host/test_store
    cc -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/src -o build/host/test_songfile host-tests/test_songfile.c firmware/src/masif_tracker.c firmware/src/masif_runtime.c firmware/src/masif_songfile.c firmware/src/masif_demo.c && build/host/test_songfile
    cc -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/src -o build/host/fuzz_song host-tests/fuzz_song.c firmware/src/masif_tracker.c firmware/src/masif_runtime.c && build/host/fuzz_song 2000
    cc -O2 -Wall -Wno-unused-function -DMASIF_ENABLE_TRACKER=1 -Ifirmware/src -o build/host/masif_link_host tests/zp12_link_host.c -lm && node tests/masif_link_test.mjs
    node tests/masif_web_test.mjs
    cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-int-to-pointer-cast -fsanitize=address,undefined -fno-sanitize-recover=all -Ifirmware/hal -o build/host/test_usb_audio host-tests/test_usb_audio.c && build/host/test_usb_audio
    python3 tests/fm1_install_test.py build/masif.fwsc

`node --experimental-websocket tests/masif_browser_test.mjs` drives the web page in a headless Firefox, and
`tests/masif_install_test.mjs` its INSTALL against a simulated FM-1; both need `(cd web && python3 -m http.server 8000)`
running.

## The web page

`web/masif/` is static: serve `web/` with any web server (`cd web && python3 -m http.server 8000`) and open
http://localhost:8000/masif/ in Chrome or Edge. The browser keeps your sample library per address, so use the same
address every time. When you change a script or the style sheet, raise the `?v=` in `web/masif/index.html` so
browsers fetch the new one.

## Source layout

- `firmware/src/masif_*`: MASIF: the song model (`masif_tracker`), the player and edit queue (`masif_runtime`), the
  song file and store, the voices adapter (`masif_fm1_adapter.c`), the screens and controls (`masif_ui.c`, the
  controls are in its head comment), the recorder (`masif_rec.c`). `zp12.c` holds `masif_main`.
- `firmware/src/sp_*`, `firmware/hal/`, `firmware/loader/`: the zp12 / SLOOP / Felucca base.
- `web/masif/`: the web page; `web/zp12link.js`: the link to the FM-1.
