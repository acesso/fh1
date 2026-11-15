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

typedef struct {
    int size;                                    // a record's bytes (at most NETREP_REC_MAX)
    // Host
    uint16_t rev;                                // changes made
    uint8_t ring[NETREP_RING][NETREP_REC_MAX];   // the latest, the newest at rev % NETREP_RING
    int car;                                     // the carousel's next record
    // Guest
    uint16_t seen;                               // the newest change applied
    bool have_seen;
    int want;                                    // the carousel's next start while catching up (-1: wait for 0)
    bool synced;                                 // the whole table seen at least once
} netrep_t;

void netrep_init(netrep_t *r, int size);
// Host: a record changed (its newest state).
void netrep_changed(netrep_t *r, const void *rec);
// Host: packs the latest changes (at most ring_max of them) and the carousel's next stretch of table (n records)
// into buf, up to max bytes. Returns the bytes used.
int netrep_pack(netrep_t *r, uint8_t *buf, int max, int ring_max, const void *table, int n);
// Guest: what netrep_pack made; apply is called for each record new to the guest. Returns the bytes read, -1 if
// the blob is short.
int netrep_unpack(netrep_t *r, const uint8_t *buf, int len, void (*apply)(const void *rec, void *user), void *user);
