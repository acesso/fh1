// The Wi-Fi link to the FH1 (udp.c; protocol: fh1_bridge_proto.h, "Over Wi-Fi"). While the bridge's Wi-Fi is
// connected it broadcasts a beacon until an FH1 says hello; then that FH1 is the peer: datagrams go to it,
// commands are taken from it only, and it is dropped after FH1B_UDP_TIMEOUT_MS of silence.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fh1_bridge_proto.h"

// What the FH1 asked, handed to BTstack's run loop (Bluepad32 is not thread-safe).
typedef void (*bridge_udp_hello_fn)(void);
typedef void (*bridge_udp_command_fn)(const uint8_t *cmd, uint16_t len);

void bridge_udp_init(bridge_udp_hello_fn hello, bridge_udp_command_fn command);   // on the run loop, once
void bridge_udp_set_beacon(const fh1b_beacon_t *b);    // what the beacon says (on the run loop)
void bridge_udp_start(void);                           // Wi-Fi has an address (any task): the socket's task starts
bool bridge_udp_linked(void);                          // an FH1 is the peer
bool bridge_udp_send(uint8_t type, const void *data, uint16_t len);   // to the peer
void bridge_udp_fails(uint32_t *n, int *err);           // failed sends since the last call, and the last errno
