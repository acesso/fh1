# FH1 extension board (Bluetooth bridge)

Firmware for an ESP32 board (ESP32-WROOM-32 or ESP32-A1S Audio Kit) that holds Bluetooth Classic controllers (DualSense and others) for
the FH1. Design, protocol and measurements: `firmware/PLAN.md`, "Extension board".

It pairs up to four controllers with Bluepad32 and serves them to the FH1 over one of two links (protocol:
`firmware/components/fh1_bridge/include/fh1_bridge_proto.h`):

- **Bluetooth LE:** a GATT service; the FH1 connects as a central. Input at 125 Hz, ~11 ms round trip.
- **Wi-Fi:** once the FH1 has given the bridge a network (Connectivity, over an encrypted BLE link), the
  bridge joins it, broadcasts a beacon on UDP 47211, and an FH1 that says hello gets the same messages as
  datagrams. The input goes when it changes (at most every 16 ms) and every 100 ms, because the WROOM's
  Wi-Fi shares the radio with Bluetooth Classic and only ~50 small datagrams a second get through. The FH1
  takes a bridge that beacons over Wi-Fi; BLE is for adding a bridge and giving it a network.

## Build

Bluepad32 is not vendored. Check out its release next to the FH1 repository and apply its BTstack
patch (inside the checkout; ESP-IDF is not touched):

```sh
cd ~/ham/FH1-ext
git clone --branch 5.0.0-beta0 --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/ricardoquesada/bluepad32.git
cd bluepad32/external/btstack && git apply ../patches/*.patch
```

Then, with ESP-IDF v6.1 (`. ~/esp/esp-idf-v6.1/export.sh`):

```sh
cd bridge
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB1 -b 230400 flash      # the board's CH340 drops data on long transfers above that
```

Another path for the checkout: `idf.py -DBLUEPAD32_ROOT=/path/to/bluepad32 build`.

**Boards:** the default build is for the ESP32-WROOM-32. For an ESP32-A1S Audio Kit (same firmware, it reports
its board as "ESP32-A1S"), build in its own directory:

```sh
idf.py -B build-a1s -DSDKCONFIG=build-a1s/sdkconfig -DBRIDGE_BOARD=A1S -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.a1s" build
idf.py -B build-a1s -p /dev/ttyUSBx -b 230400 flash
```

The A1S Audio Kit's USB-serial is a CP2102 on its "UART" micro-USB socket (the other socket is power only).

**Its name on the network:** `fh1-bridge-<last four hex digits of its Bluetooth address>.local` (mDNS; the
A1S is `fh1-bridge-cb5a.local`), so its address does not matter. mDNS is the FH1 firmware's copy of
espressif/mdns (`firmware/managed_components`): build the FH1 once before the bridge.

**Updates over Wi-Fi (OTA):** a bridge on Wi-Fi takes its firmware at `POST http://<bridge>/ota` with the
header `X-FH1-Bridge-Code` (six digits, made once by the bridge). Both are in the bridge's About on the FH1
("OTA") and in the FH1's dev console stats (`bridges`). `GET http://<bridge>/` says what it runs and from
which slot. The new firmware is kept after 10 s of running; a crash before that rolls it back.

```sh
curl -H "X-FH1-Bridge-Code: <code>" --data-binary @build-a1s/fh1_bridge.bin http://fh1-bridge-cb5a.local/ota
```

The partition table has two 1.5 MB app slots (`partitions.csv`); NVS is at 0x9000 (pairings and Wi-Fi).

**Console:** 115200 baud. Open the port with DTR and RTS released (`dtr=False, rts=False` in pyserial):
the default open holds the ESP32 in reset.

## Pairing

Start pairing from the FH1 (a bridge's Devices or Connectivity: "Pair a controller"; 60 s), then: a
DualSense, hold Create + PS until the light bar flashes fast; a Joy-Con or Switch Pro, hold its sync button.
Scanning stops as soon as one is ready (it costs the connected ones radio time), and the bridge's Wi-Fi is
off meanwhile. At power-up the bridge scans only when no controller is
paired. Paired controllers are kept (NVS) and come back with a press of PS / Home.

## Licences

Bluepad32: Apache 2.0. BTstack (which Bluepad32 uses on the ESP32): free for non-commercial use only.
The original flash of the board is backed up outside the repository (`~/ham/FH1-backups/`).
