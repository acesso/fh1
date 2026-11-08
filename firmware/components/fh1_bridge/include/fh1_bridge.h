// Extension boards (Bluetooth bridges), the FH1's side. Two links, one at a time per bridge:
//   "bridge0"  the FH1's Bluetooth LE (NimBLE, central only): while it is enabled, the FH1 finds bridges by
//              their service and connects to the ones it knows (up to FH1_BRIDGES_LINKED at once).
//   "bridge1"  Wi-Fi: bridges on the FH1's network (their Wi-Fi set over BLE once) are linked over UDP, so
//              the FH1's Bluetooth can stay off (~27 KB of internal RAM). Wi-Fi goes first; Bluetooth is for
//              adding a bridge and giving it a network (firmware/PLAN.md).
// Either way their controllers become gamepads (fh1_gamepad.h).
// Bridges are remembered with an alias (NVS). A new bridge is added when none is known yet, or during a
// search the user starts. See firmware/PLAN.md, "Extension board", and fh1_bridge_proto.h.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fh1_devices.h"
#include "fh1_bridge_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FH1_BRIDGES_KNOWN 4            // remembered
#define FH1_BRIDGES_LINKED 2           // connected at once (CONFIG_BT_NIMBLE_MAX_CONNECTIONS)

extern const fh1_driver_t fh1_bridge_driver;          // BLE
extern const fh1_driver_t fh1_bridge_net_driver;      // Wi-Fi (UDP)
extern const fh1_driver_t fh1_bridge_audio_driver;    // audio.out: a bridge's speaker ("baudio0", "baudio1")

enum { FH1_BRIDGE_LINK_NONE = 0, FH1_BRIDGE_LINK_BLE = 1, FH1_BRIDGE_LINK_WIFI = 2 };

typedef struct {
    char alias[24];
    uint8_t addr[6];                   // its BLE address, most significant byte first (as printed)
    bool connected;
    uint8_t link;                      // FH1_BRIDGE_LINK_*
    uint8_t ip[4];                     // over Wi-Fi
    bool encrypted;                    // the link is encrypted: only while Wi-Fi credentials are sent
    bool have_info, have_status;
    fh1b_info_t info;
    fh1b_status_t status;
    uint8_t ndev;
    fh1b_device_t dev[FH1B_MAX_DEVICES];
    float msgs_per_s;                  // input notifications received
    uint32_t lost;                     // since connecting (gaps in the sequence)
    uint16_t rtt_ms, rtt_max_ms;       // last ping, worst in the last 5 s
    uint16_t interval_x100;            // BLE: the connection interval, ms x 100
    uint16_t mtu;                      // BLE
    int8_t rssi;                       // dBm, 0 unknown: BLE's, or over Wi-Fi the bridge's Wi-Fi signal
    uint16_t spread_ms;                // the input's one-way delay variation in the last 5 s
    uint8_t nboard;                    // the board's own devices (keys, LED, codec), protocol 5
    fh1b_board_dev_t board[FH1B_BOARD_MAX];
} fh1_bridge_view_t;

bool fh1_bridge_on(void);                             // Bluetooth running
bool fh1_bridge_wifi_link_on(void);                   // the Wi-Fi link's device running
int fh1_bridge_count(void);                           // remembered bridges
bool fh1_bridge_get(int i, fh1_bridge_view_t *out);
void fh1_bridge_rename(int i, const char *alias);     // saved
void fh1_bridge_forget(int i);                        // disconnect and drop it
void fh1_bridge_search(int seconds);                  // look for new bridges and add them
bool fh1_bridge_searching(void);

// Commands to bridge i (false when it is not connected). pad: the bridge's slot (fh1b_device_t.id).
bool fh1_bridge_pair(int i, int seconds);
int fh1_bridge_pair_all(int seconds);                 // every connected bridge waits for a controller; how many
bool fh1_bridge_forget_controllers(int i);
bool fh1_bridge_rumble(int i, int pad, uint8_t weak, uint8_t strong, uint16_t ms);
bool fh1_bridge_lightbar(int i, int pad, uint8_t r, uint8_t g, uint8_t b);
bool fh1_bridge_request_status(int i);                // the answer shows in the view (have_status)
bool fh1_bridge_board_set(int i, uint8_t id, uint8_t value);   // a board device (fh1b_board_dev_t.id): a light on/off
const char *fh1_bridge_audio_device(int i);           // bridge i's speaker as a sound device id ("baudio0"), or NULL
// A board device's controls (protocol 7), as the bridge last reported them, in order; and setting one (the
// bridge applies and saves it, and reports it back).
int fh1_bridge_controls(int i, uint8_t dev, fh1b_ctrl_t *out, int max);
bool fh1_bridge_control_set(int i, uint8_t dev, uint8_t id, int16_t value);
// The sound sent to bridges (saved): 48 kHz stereo (192 KB/s), 24 kHz stereo (96 KB/s, the default), 24 kHz mono
// (48 KB/s).
enum { FH1_BRIDGE_AUDIO_48K_STEREO, FH1_BRIDGE_AUDIO_24K_STEREO, FH1_BRIDGE_AUDIO_24K_MONO, FH1_BRIDGE_AUDIO_QUALITIES };
uint8_t fh1_bridge_audio_quality(void);
void fh1_bridge_set_audio_quality(uint8_t q);
// The bridge's Wi-Fi (its Connectivity page). For the credentials the link is encrypted, they are sent, and
// the link drops and comes back unencrypted (about a second without the bridge's controllers): over BLE
// only (wifi_set is false over Wi-Fi).
bool fh1_bridge_wifi_set(int i, const char *ssid, const char *password);
bool fh1_bridge_wifi_enable(int i, bool on);
bool fh1_bridge_wifi_forget(int i);

// A controller's look for its icon (FH1_PAD_* and 0xRRGGBB), as its gamepad has (fh1_gamepad_info_t).
void fh1_bridge_pad_look(const fh1b_device_t *d, uint8_t *kind, uint32_t *rgb);

#ifdef __cplusplus
}
#endif
