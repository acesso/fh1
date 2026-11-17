// A host's table of records (a game world's changes) copied to its network guests, for every game of the engine.
// The SDK's network play carries the host's state to its guests as one blob, the newest winning and nothing resent
// (fh1_net_state_send). So each blob carries part of the table: the latest changes (a ring: a guest keeps up while
// it misses a few blobs) and the next stretch of the whole table, going round (a carousel: a guest that joins late
// or missed too much catches up), with no answers needed. Records are a game's own, of a fixed size, the newest
// state of whatever they stand for (applying one twice, or out of order with the carousel, comes to the same).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NETREP_RING 64
#define NETREP_REC_MAX 16
#define NETREP_RECENT 32                         // the ring carries the changes of the last this many packs
#define NETREP_TABLE_MAX 2048                    // records in a table, at most

typedef struct {
    int size;                                    // a record's bytes (at most NETREP_REC_MAX)
    // Host
    uint16_t rev;                                // changes made
    uint8_t ring[NETREP_RING][NETREP_REC_MAX];   // the latest, the newest at rev % NETREP_RING
    int car;                                     // the carousel's next record
    uint16_t hist[NETREP_RECENT];                // rev at each of the last packs
    int nhist;
    // Guest
    uint16_t seen;                               // the newest change applied
    bool have_seen;
    uint8_t seen_rec[NETREP_TABLE_MAX / 8];      // the table's records seen (a bit each), while catching up
    int nseen;
    bool synced;                                 // the whole table seen at least once (lost blobs only delay it)
} netrep_t;

void netrep_init(netrep_t *r, int size);
// Host: a record changed (its newest state).
void netrep_changed(netrep_t *r, const void *rec);
// Host: packs the changes of the last NETREP_RECENT packs (at most ring_max of them) and the carousel's next
// stretch of table (n records, at most car_max of them) into buf, up to max bytes. Returns the bytes used. Small
// is better (Wi-Fi loses big datagrams first): a game sends a long stretch of carousel only while a guest is
// catching up (just joined), a few records otherwise; a guest that missed more than the recent changes catches up
// from the carousel.
int netrep_pack(netrep_t *r, uint8_t *buf, int max, int ring_max, int car_max, const void *table, int n);
// Guest: what netrep_pack made; apply is called for each record new to the guest. Returns the bytes read, -1 if
// the blob is short.
int netrep_unpack(netrep_t *r, const uint8_t *buf, int len, void (*apply)(const void *rec, void *user), void *user);
