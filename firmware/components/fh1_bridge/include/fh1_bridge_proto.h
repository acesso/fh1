// The FH1 bridge protocol: shared by the FH1 (components/fh1_bridge) and the bridge firmware (bridge/, which
// includes this file). All fields little-endian, packed. Over BLE each message is the value of one
// characteristic of the bridge's service; over Wi-Fi each is a UDP datagram with a short header (below).
// See firmware/PLAN.md, "Extension board".
#pragma once
#include <stdint.h>

#define FH1B_PROTO_VERSION 7       // 2: devices carry their colour (40-byte entries; 1 had 36). 3: the Wi-Fi link.
                                   // 4: motion over Wi-Fi on request, adaptive triggers, player LEDs as a feature.
                                   // 5: the board's own devices (keys, LED, codec) and key events.
                                   // 6: sound to a board's codec over Wi-Fi (FH1B_UDP_AUDIO), its SD card slot.
                                   // 7: a board device's controls (FH1B_CMD_CTRLS, fh1b_ctrl_t)

// The service and its characteristics (128-bit UUIDs; xx = 00 service, 01..05 characteristics):
// F1B100xx-5A1E-4C6B-9E2A-6F48315A0001
#define FH1B_UUID_BYTES(xx) \
    0x01, 0x00, 0x5A, 0x31, 0x48, 0x6F, 0x2A, 0x9E, 0x6B, 0x4C, 0x1E, 0x5A, (xx), 0x00, 0xB1, 0xF1
enum { FH1B_CHR_SERVICE = 0, FH1B_CHR_INFO = 1, FH1B_CHR_DEVICES = 2, FH1B_CHR_INPUT = 3, FH1B_CHR_COMMAND = 4, FH1B_CHR_EVENT = 5 };

#define FH1B_MAX_DEVICES 4

// info (read)
typedef struct __attribute__((packed)) {
    uint8_t proto;                 // FH1B_PROTO_VERSION
    uint8_t max_devices;
    uint16_t rate_hz;              // input notifications a second
    uint32_t caps;                 // what the bridge can carry: FH1B_CAP_*
    char board[16];                // "ESP32-WROOM-32"
    char firmware[16];             // build id
} fh1b_info_t;
enum { FH1B_CAP_GAMEPAD = 1u << 0, FH1B_CAP_MOTION = 1u << 1, FH1B_CAP_RUMBLE = 1u << 2, FH1B_CAP_LIGHTBAR = 1u << 3,
       FH1B_CAP_WIFI_LINK = 1u << 4, FH1B_CAP_TRIGGERS = 1u << 5 /* DualSense adaptive triggers */,
       FH1B_CAP_LEDS = 1u << 6 /* player LEDs */ };

// devices (read, notify): a count, then one entry per attached device
enum { FH1B_CLASS_GAMEPAD = 1 };
enum { FH1B_STATE_ABSENT = 0, FH1B_STATE_READY = 1 };
typedef struct __attribute__((packed)) {
    uint8_t id;                    // the bridge's slot, 0..max_devices-1; stays while connected
    uint8_t cls;                   // FH1B_CLASS_*
    uint8_t state;                 // FH1B_STATE_*
    uint8_t model;                 // Bluepad32's controller type (45: DualSense)
    uint8_t battery;               // 0 unknown, 1 empty .. 255 full
    uint8_t features;              // FH1B_CAP_* this device has
    uint16_t vid, pid;
    uint8_t addr[6];
    char name[20];
    uint8_t rgb[3];                // its light: the light bar the bridge set (0, 0, 0: none or unknown)
    uint8_t reserved;
} fh1b_device_t;                   // 40 bytes
#define FH1B_DEVICE_V1_SIZE 36     // protocol 1: without rgb
typedef struct __attribute__((packed)) {
    uint8_t count;
    fh1b_device_t dev[FH1B_MAX_DEVICES];
} fh1b_devices_t;

// input (notify): a header, then the state of every ready gamepad
typedef struct __attribute__((packed)) {
    uint8_t id;
    uint8_t dpad;                  // bits: up 1, down 2, right 4, left 8
    uint16_t buttons;              // FH1B_BTN_*
    uint8_t misc;                  // FH1B_MISC_*
    int16_t lx, ly, rx, ry;        // -512..511, y down positive
    uint16_t l2, r2;               // 0..1023
    int16_t gyro[3];               // rad/s x 1000
    int16_t accel[3];              // m/s^2 x 100
    uint8_t battery;
} fh1b_pad_t;                      // 30 bytes
typedef struct __attribute__((packed)) {
    uint8_t seq;                   // +1 per notification: the FH1 counts gaps as losses
    uint8_t count;
    uint32_t time_ms;              // the bridge's clock
    fh1b_pad_t pad[FH1B_MAX_DEVICES];
} fh1b_input_t;
#define FH1B_INPUT_SIZE(n) (6 + 30 * (n))
// The buttons as Bluepad32 reports them (Xbox-style names: A is the bottom face button).
enum {
    FH1B_BTN_A = 1 << 0, FH1B_BTN_B = 1 << 1, FH1B_BTN_X = 1 << 2, FH1B_BTN_Y = 1 << 3,
    FH1B_BTN_L1 = 1 << 4, FH1B_BTN_R1 = 1 << 5, FH1B_BTN_L2 = 1 << 6, FH1B_BTN_R2 = 1 << 7,
    FH1B_BTN_L3 = 1 << 8, FH1B_BTN_R3 = 1 << 9,
};
enum { FH1B_MISC_SYSTEM = 1 << 0, FH1B_MISC_SELECT = 1 << 1, FH1B_MISC_START = 1 << 2, FH1B_MISC_CAPTURE = 1 << 3 };
enum { FH1B_DPAD_UP = 1, FH1B_DPAD_DOWN = 2, FH1B_DPAD_RIGHT = 4, FH1B_DPAD_LEFT = 8 };

// command (write): an op, then its arguments
enum {
    FH1B_CMD_PAIR = 1,             // u8 seconds: scan for a new controller (0: stop)
    FH1B_CMD_FORGET_ALL = 2,       // disconnect and forget every paired controller
    FH1B_CMD_RUMBLE = 3,           // u8 id, u8 weak, u8 strong, u16 ms
    FH1B_CMD_LIGHTBAR = 4,         // u8 id, u8 r, g, b
    FH1B_CMD_LEDS = 5,             // u8 id, u8 mask (player LEDs)
    FH1B_CMD_RATE = 6,             // u16 hz (25..250)
    FH1B_CMD_PING = 7,             // u32 token: answered at once with FH1B_EVT_PONG
    FH1B_CMD_STATUS = 8,           // answered with FH1B_EVT_STATUS
    FH1B_CMD_WIFI_SET = 9,         // u8 ssid length, ssid, u8 password length, password: saved, then connects.
                                   // Refused unless the link is encrypted (so never over Wi-Fi).
    FH1B_CMD_WIFI_ENABLE = 10,     // u8 on (saved)
    FH1B_CMD_WIFI_FORGET = 11,     // drop the saved network
    FH1B_CMD_MOTION = 12,          // u8 on: over Wi-Fi, a change of motion is a change to send (off: motion rides
                                   // along with other changes, at least every 100 ms). Over BLE it always flows.
    FH1B_CMD_TRIGGER = 13,         // u8 id, u8 side (0 L2, 1 R2), u8 effect FH1B_TRIG_*, u8 a, b, c (see below)
    FH1B_CMD_BOARD = 14,           // answered with FH1B_EVT_BOARD, then FH1B_EVT_KEYS for each keys device
    FH1B_CMD_BOARD_SET = 15,       // u8 board device id, u8 value (a light: 0 off, 1 on)
    FH1B_CMD_CTRLS = 16,           // u8 board device id: answered with an FH1B_EVT_CTRL per control it has
    FH1B_CMD_CTRL_SET = 17,        // u8 board device id, u8 control id, i16 value: applied, saved, answered (FH1B_EVT_CTRL)
};
// Adaptive trigger effects (DualSense). FEEDBACK: resists from position a (0-9) with strength b (0-8). WEAPON:
// a click between positions a (2-7) and b (a+1-8), strength c (0-8). VIBRATION: from position a (0-9),
// amplitude b (0-8), frequency c (Hz).
enum { FH1B_TRIG_OFF = 0, FH1B_TRIG_FEEDBACK = 1, FH1B_TRIG_WEAPON = 2, FH1B_TRIG_VIBRATION = 3 };

// event (notify): a type, then its data
enum {
    FH1B_EVT_PONG = 1,             // u32 token (as sent); protocol 3 adds i8: the bridge's Wi-Fi signal (dBm, 0 none)
    FH1B_EVT_PAIRING = 2,          // u8 1 started, 0 ended
    FH1B_EVT_LOG = 3,              // text
    FH1B_EVT_STATUS = 4,           // fh1b_status_t
    FH1B_EVT_BOARD = 5,            // u8 count, then fh1b_board_dev_t each: the board's own devices
    FH1B_EVT_KEYS = 6,             // u8 board device id, u32 keys held (bit i: key i); sent when they change
    FH1B_EVT_CTRL = 7,             // fh1b_ctrl_t: one control of a board device, as it is now
};

// A board device's control (protocol 7): what the FH1 draws in that device's view, generically, as it does
// its own devices' settings. The bridge keeps the values (saved) and applies them at once.
enum { FH1B_CTRL_TOGGLE = 0,       // 0 off, 1 on
       FH1B_CTRL_CHOICE = 1,       // 0..max: opts holds the names, separated by '|'
       FH1B_CTRL_RANGE = 2 };      // min..max in steps of 1; opts holds the unit ("dB")
typedef struct __attribute__((packed)) {
    uint8_t dev;                   // the board device (fh1b_board_dev_t.id)
    uint8_t id;                    // the control, 0.. (its order in the view)
    uint8_t type;                  // FH1B_CTRL_*
    int16_t min, max, value;
    char name[20];                 // English, translated on the FH1 when it knows the words
    char opts[40];
} fh1b_ctrl_t;                     // 69 bytes
#define FH1B_CTRLS_MAX 10

// The board's own devices (protocol 5): what the board has besides the radio (an audio kit's codec, keys...).
enum { FH1B_CLASS_AUDIO_OUT = 2, FH1B_CLASS_AUDIO_IN = 3, FH1B_CLASS_KEYS = 4, FH1B_CLASS_LIGHT = 5, FH1B_CLASS_STORAGE = 6 };
enum { FH1B_BOARD_STREAMS = 1 << 0,    // audio carried over the link (audio out: FH1B_UDP_AUDIO, Wi-Fi only)
       FH1B_BOARD_PLUGGED = 1 << 1,    // audio out: headphones in; storage: a card in
       FH1B_BOARD_PLAYING = 1 << 2 };  // audio out: sound arriving now
// count: keys, how many; audio, channels; light, 0 off / 1 on; storage, the card's size in GB (0 none).
typedef struct __attribute__((packed)) {
    uint8_t id;                    // 0.. (its own numbering, not the controllers')
    uint8_t cls;                   // FH1B_CLASS_*
    uint8_t flags;                 // FH1B_BOARD_*
    uint8_t count;                 // keys: how many; audio: channels; light: 0 off, 1 on
    char name[16];                 // "ES8388", "REC MODE PLAY.."
} fh1b_board_dev_t;                // 20 bytes
#define FH1B_BOARD_MAX 8

// The bridge's own state, for the FH1's About and Storage pages of a bridge (after the event's type byte).
typedef struct __attribute__((packed)) {
    uint32_t uptime_s;
    uint32_t heap_free, heap_min;  // bytes, internal RAM
    uint16_t cpu_mhz;
    uint8_t flash_mb;
    uint8_t paired;                // controllers it remembers (link keys)
    uint16_t nvs_used, nvs_total;  // NVS entries
    uint32_t app_kb, app_part_kb;  // the running firmware's size and its partition's
    uint8_t bt_addr[6];            // its Bluetooth address (Classic and LE share it)
    // Added in the same protocol version: older bridges send only the 34 bytes above.
    uint32_t heap_total;           // bytes, internal RAM
    uint8_t wifi;                  // FH1B_WIFI_*
    int8_t wifi_rssi;              // dBm while connected
    uint8_t wifi_ip[4];
    uint8_t encrypted;             // the link to this FH1 is encrypted (credentials may be sent)
    char wifi_ssid[33];            // the saved network ("" none)
    uint32_t ota_code;             // protocol 6: the code for POST /ota on the bridge (0: no updates over Wi-Fi)
} fh1b_status_t;                   // 82 bytes
#define FH1B_STATUS_V1_SIZE 34
enum { FH1B_WIFI_OFF = 0, FH1B_WIFI_NO_NETWORK = 1, FH1B_WIFI_CONNECTING = 2, FH1B_WIFI_CONNECTED = 3 };

// ── Over Wi-Fi (protocol 3) ──
// When the bridge and the FH1 are on the same network, the same messages go as UDP datagrams, both ends on
// port FH1B_UDP_PORT, not encrypted (nothing secret travels this way: Wi-Fi credentials go only over an
// encrypted BLE link). Each datagram: 'F', '1', a type, then the message. Types FH1B_CHR_DEVICES, _INPUT,
// _COMMAND and _EVENT carry those messages; FH1B_CHR_INFO the info. And:
//   FH1B_UDP_BEACON  bridge, broadcast every 2 s while no FH1 is linked to it: fh1b_beacon_t
//   FH1B_UDP_HELLO   FH1 to bridge: link to me. Answered with info and devices; the input then flows to the
//                    sender at the set rate, and commands are taken from it only.
//   FH1B_UDP_BYE     FH1 to bridge: unlinking
// Either end drops the link after FH1B_UDP_TIMEOUT_MS without a datagram from the other (the FH1 pings
// every second; the bridge sends input when it changes, at most every 16 ms, and at least every 100 ms:
// its Wi-Fi shares the radio with the controllers).
#define FH1B_UDP_PORT 47211
#define FH1B_UDP_TIMEOUT_MS 3000
#define FH1B_UDP_HDR 3
enum { FH1B_UDP_BEACON = 0x10, FH1B_UDP_HELLO = 0x11, FH1B_UDP_BYE = 0x12 };
typedef struct __attribute__((packed)) {
    uint8_t addr[6];               // its Bluetooth address, as fh1b_status_t.bt_addr (its BLE address too)
    fh1b_info_t info;
} fh1b_beacon_t;

// Sound to a board's audio out (protocol 6), FH1 to bridge over Wi-Fi only (BLE is too narrow): type
// FH1B_UDP_AUDIO, this header, then the samples. IMA ADPCM, mono, 4 bits a sample (two a byte, the first in
// the low nibble); each packet starts from the predictor and step index in its header, so a lost one costs
// only its own samples (the bridge plays silence for them, by the sequence). The bridge buffers ~200 ms
// before playing, and stops (amplifier off) after a second without packets.
enum { FH1B_UDP_AUDIO = 0x13 };
enum { FH1B_AUDIO_IMA_ADPCM = 1,   // mono, 4 bits a sample: ~11 KB/s at 22 kHz, audible noise on loud sound
       FH1B_AUDIO_PCM16 = 2,       // mono, 16-bit little-endian: ~48 KB/s at 24 kHz
       FH1B_AUDIO_PCM16_STEREO = 3 };   // L, R pairs, 16-bit: 192 KB/s at 48 kHz (samples counts the pairs)
typedef struct __attribute__((packed)) {
    uint8_t id;                    // the board device (FH1B_CLASS_AUDIO_OUT)
    uint8_t codec;                 // FH1B_AUDIO_*
    uint16_t seq;                  // +1 a packet
    uint16_t rate;                 // Hz (8000..48000)
                                   // (predictor and index: ADPCM only)
    uint16_t samples;              // in this packet (even)
    int16_t predictor;             // ADPCM state at the first sample
    uint8_t index;
    uint8_t volume;                // 0-100
} fh1b_audio_hdr_t;                // 12 bytes
#define FH1B_AUDIO_SAMPLES 1024    // the most samples a packet carries (ADPCM: 1024 in 512 bytes)
#define FH1B_AUDIO_PCM_SAMPLES 700 // PCM16 packets: 1400 bytes, under one Wi-Fi frame
#define FH1B_AUDIO_STEREO_FRAMES 350   // PCM16 stereo packets: 1400 bytes
