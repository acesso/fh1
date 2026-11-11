# Web simulator and site

The site at **https://acesso.github.io/fh1/** is the FH1 landing page plus a simulator that runs the real
firmware in the browser. The binary is the same one the FH1 board runs, on an emulated ESP32-S3:
[esp32sim](https://github.com/mikroverk/esp32sim) (MIT), compiled to WebAssembly, with an `fh1` board model:

- the ST7789 display on SPI2;
- the two MCP23017 expanders (buttons and their LEDs, amplifier enable);
- ES8311 and PCF8563 stubs;
- Menu on GPIO0.

Not modelled yet: the micro-SD slot, the radio, the IMU, Wi-Fi.

| File | What |
|---|---|
| `esp32sim.pin` | The upstream esp32sim repository and commit the site is built from |
| `esp32sim.patch` | Our changes to it: the `fh1` board model, its CLI help line, the page's FH1 display panel |
| `build-flash.sh` | The 16 MB flash image: firmware build + apps partition with packages installed |
| `build-site.sh` | Assembles the site: landing page, emulator page, mask ROM, flash image, `build.json` |
| `site/` | The landing page (Portuguese and English, dark, light and auto themes) |

## Building it

```sh
# 1. The firmware for the FH1 board (firmware/README.md), and the packages to install
firmware/tools/build-board.sh fh1 out/firmware-fh1      # with ESP-IDF's environment (export.sh)
firmware/tools/build-packages.sh out/packages

# 2. The flash image (pip install esptool littlefs-python==0.15.0)
simulator/build-flash.sh out/firmware-fh1 fh1.bin out/packages/*.fpk

# 3. esp32sim at the pinned commit, patched, built for the browser (Rust with the wasm32-unknown-unknown target)
read url rev < simulator/esp32sim.pin
git clone "$url" esp32sim && git -C esp32sim checkout "$rev" && git -C esp32sim apply ../simulator/esp32sim.patch
(cd esp32sim && tools/wasm-build.sh)

# 4. The site, then serve it (any static server; file:// does not work)
simulator/build-site.sh esp32sim fh1.bin site
python3 -m http.server -d site 8790      # http://127.0.0.1:8790/
```

The emulator page opens at `run.html?wasm&fw=fh1`. Keys go to the UART0 console (`firmware/main/serial_keys.c`):

| Key | Button |
|---|---|
| `W` `A` `S` `D` | d-pad |
| `J` / `K` | A / B |
| `U` / `I` | X / Y |
| Enter / space | Start / Select |
| `M` | Menu |

## Publishing

`.github/workflows/pages.yml` builds everything on each push and publishes the site from `main`:

- the firmware for both boards (`firmware/tools/build-board.sh fh1|guition`) in the `espressif/idf:v6.1` container;
- the packages and sound packs (`firmware/tools/build-packages.sh`);
- the flash image, esp32sim's WebAssembly module and the site.

Pages takes it from GitHub Actions (Settings → Pages → Source: GitHub Actions). Pushes to other branches run
the same build without publishing. Each run keeps the firmware images (`firmware-fh1`, `firmware-guition`)
and the packages as artifacts.
