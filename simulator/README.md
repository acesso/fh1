# Web simulator and site

The site at **https://acesso.github.io/fh1/** is the FH1 landing page plus a simulator that runs the real
firmware in the browser. The binary is the same one the FH1 board runs, on an emulated ESP32-S3:
[esp32sim](https://github.com/mikroverk/esp32sim) (MIT), compiled to WebAssembly, with an `fh1` board model:

- the ST7789 display on SPI2;
- the two MCP23017 expanders: A, B, X, Y, Start and Select on #1's GPA0-5, their LEDs on GPB0-3, the amplifier
  enable on #2;
- the thumbstick on the ADC (GPIO2 and GPIO4, 1.65 V centred, low = left / up);
- Menu on GPIO0;
- the I2S sound, played through Web Audio;
- ES8311 and PCF8563 stubs.

Not modelled yet: the micro-SD slot, the radio, the IMU, Wi-Fi.

| File | What |
|---|---|
| `esp32sim.pin` | The upstream esp32sim repository and commit the site is built from |
| `esp32sim.patch` | Our changes to it: the `fh1` board model; board keys (`key`) and indicator lights (`lights`) in the board interface; the page messages `press`, `adc` and `lights`; `press NAME` for board keys in scripts; the workbench's FH1 display panel |
| `build-flash.sh` | The 16 MB flash image: firmware build + apps partition with packages installed |
| `build-site.sh` | Assembles the site: landing page, emulator page, mask ROM, flash image, `build.json` |
| `site/index.html` | The landing page (Portuguese and English, dark, light and auto themes), with the 3D view of the V1 build (`hardware/layout/concept-render.html`, published as `hardware-3d.html` with its own copy of three.js; without WebGL it shows the static render) |
| `site/fh1-device.css` | The FH1's front face to scale, from the layout's millimetres (80 × 120 mm board): the landing page's toy and the simulator's device |
| `site/play.html` | The simulator: the FH1 drawn as the device, its screen, buttons with their lights, stick and sound; 🛠 opens the details (status, speed, restart, your own firmware, the UART0 console) |

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

`play.html` is the simulator; `?panels` opens it with the details shown. Its controls reach the firmware the way
the hardware does: the buttons through the MCP23017, the stick through the ADC, Menu through GPIO0.

| Key | Control |
|---|---|
| arrows, `W` `A` `S` `D` | thumbstick |
| `J` / `K` | A / B |
| `U` / `I` | X / Y |
| Enter / space | Start / Select |
| `M`, Esc | Menu |

esp32sim's own workbench, `run.html?wasm&fw=fh1`, stays on the site for debugging: registers, console tabs, the
UART0 keys of `firmware/main/serial_keys.c`. Native runs take the same keys in scripts (`press a`, `adc 4 3.1`).

## Publishing

A release publishes the site: a `vX.Y.Z` tag on `main` runs `.github/workflows/pages.yml`, which builds

- the firmware for both boards (`firmware/tools/build-board.sh fh1|guition`) in the `espressif/idf:v6.1` container;
- the packages and sound packs (`firmware/tools/build-packages.sh`);
- the flash image, esp32sim's WebAssembly module and the site;

then creates the GitHub release and deploys the site. Pages takes it from GitHub Actions (Settings → Pages →
Source: GitHub Actions), and the `github-pages` environment allows `v*` tags to deploy (Settings → Environments).
Run by hand on a branch (Actions → Site → Run workflow), the workflow builds everything without releasing.
