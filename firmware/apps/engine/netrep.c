#include "netrep.h"
#include <string.h>

// The packed form: the newest change's number (2 bytes), the ring's records sent (1), the table's size (2), the
// carousel's start (2) and records sent (1); then the ring's records, oldest first (the last one is change `rev`),
// then the carousel's.
#define HEAD 8

void netrep_init(netrep_t *r, int size)
{
    memset(r, 0, sizeof(*r));
    r->size = size > NETREP_REC_MAX ? NETREP_REC_MAX : size;
}

void netrep_changed(netrep_t *r, const void *rec)
{
    r->rev++;
    memcpy(r->ring[r->rev % NETREP_RING], rec, (size_t)r->size);
}

int netrep_pack(netrep_t *r, uint8_t *buf, int max, int ring_max, int car_max, const void *table, int n)
{
    if (max < HEAD) return 0;
    int room = (max - HEAD) / r->size;
    // The ring: the changes since the oldest of the last NETREP_RECENT packs (all of them until there were that many).
    const uint16_t since = r->nhist < NETREP_RECENT ? 0 : r->hist[r->nhist % NETREP_RECENT];
    r->hist[r->nhist % NETREP_RECENT] = r->rev;
    r->nhist++;
    int nr = (uint16_t)(r->rev - since);
    if (nr > NETREP_RING) nr = NETREP_RING;
    if (nr > ring_max) nr = ring_max;
    if (nr > room) nr = room;
    room -= nr;
    if (r->car >= n) r->car = 0;
    int nc = n - r->car;
    if (nc > room) nc = room;
    if (nc > car_max) nc = car_max;
    if (nc > 255) nc = 255;
    if (nr > 255) nr = 255;
    buf[0] = (uint8_t)r->rev, buf[1] = (uint8_t)(r->rev >> 8), buf[2] = (uint8_t)nr;
    buf[3] = (uint8_t)n, buf[4] = (uint8_t)(n >> 8);
    buf[5] = (uint8_t)r->car, buf[6] = (uint8_t)(r->car >> 8), buf[7] = (uint8_t)nc;
    uint8_t *p = buf + HEAD;
    for (int i = 0; i < nr; i++, p += r->size) memcpy(p, r->ring[(uint16_t)(r->rev - nr + 1 + i) % NETREP_RING], (size_t)r->size);
    memcpy(p, (const uint8_t *)table + (size_t)r->car * r->size, (size_t)nc * r->size);
    p += nc * r->size;
    r->car += nc;
    return (int)(p - buf);
}

int netrep_unpack(netrep_t *r, const uint8_t *buf, int len, void (*apply)(const void *rec, void *user), void *user)
{
    if (len < HEAD) return -1;
    const uint16_t rev = (uint16_t)(buf[0] | buf[1] << 8);
    const int nr = buf[2], n = buf[3] | buf[4] << 8, start = buf[5] | buf[6] << 8, nc = buf[7];
    if (len < HEAD + (nr + nc) * r->size) return -1;
    const uint8_t *p = buf + HEAD;
    // The ring: changes newer than the newest applied (all of them the first time).
    for (int i = 0; i < nr; i++, p += r->size) {
        const uint16_t k = (uint16_t)(rev - nr + 1 + i);
        if (r->have_seen && (int16_t)(k - r->seen) <= 0) continue;
        apply(p, user);
    }
    r->seen = rev;
    r->have_seen = true;
    // The carousel: every record (the newest state, so applying again does no harm); caught up once every record of
    // the table has come, in whatever blobs (a lost one only delays it).
    for (int i = 0; i < nc; i++, p += r->size) {
        apply(p, user);
        const int k = start + i;
        if (!r->synced && k < NETREP_TABLE_MAX && !(r->seen_rec[k >> 3] & (1 << (k & 7)))) {
            r->seen_rec[k >> 3] |= (uint8_t)(1 << (k & 7));
            r->nseen++;
        }
    }
    if (!r->synced && (n == 0 || r->nseen >= (n < NETREP_TABLE_MAX ? n : NETREP_TABLE_MAX))) r->synced = true;
    return (int)(p - buf);
}
