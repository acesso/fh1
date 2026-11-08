// Users: the people on this FH1 (firmware/docs/users.md, phase U1).
//
// Every user has a key pair (Ed25519) made on the unit; the user id is the first 10 bytes of SHA-256 of the
// public key in base32 (16 characters), so ids never collide between units. The short name is unique on this
// unit only; elsewhere it shows with a tag, the id's first 4 characters: "igor#K7Q2". A user's public profile is
// a small record signed with their key (CBOR: version, key, name, avatar, rev, unit), so it can travel between
// units: only the key's holder can change it, and the highest rev wins.
//
// On a unit: one owner (the first user made; ownership can be handed to another local user), local users, and
// visitors (never the owner; removed with their data). A controller can belong to a user: when it plays, its
// user is that player. Player 1's user is the interface user: apps keep their data in that user's folder
// (fh1_data_dir, fh1_storage.h). With no controller of theirs, player 1 is the user picked with
// fh1_users_use (the owner after start-up).
//
// A PIN (optional, per user) locks the private key (Argon2id, then XChaCha20-Poly1305): actions that sign or
// act as the user ask for it. A backup file on the SD card, locked with a passphrase, moves an identity to
// another FH1. Private keys never leave the unit any other way.
//
// Call from the UI task (or any one task at a time).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FH1_USERS_MAX 16
#define FH1_USER_ID_LEN 16              // base32 characters
#define FH1_USER_NAME_MIN 3
#define FH1_USER_NAME_MAX 16
#define FH1_USER_AVATAR_MAX 24          // the avatar code (fh1_avatar.h); empty: drawn from the id
#define FH1_USER_PIN_MIN 4
#define FH1_USER_PIN_MAX 8
#define FH1_USERS_BACKUP_DIR "/sd/users"

typedef enum { FH1_USER_LOCAL, FH1_USER_VISITOR } fh1_user_kind_t;

typedef struct {
    char id[FH1_USER_ID_LEN + 1];
    char name[FH1_USER_NAME_MAX + 1];
    char tag[5];                        // the id's first 4 characters
    uint8_t pk[32];
    uint32_t rev;
    uint8_t avatar[FH1_USER_AVATAR_MAX];
    uint8_t avatar_len;
    uint8_t kind;                       // fh1_user_kind_t
    bool owner;
    bool pin;                           // the key is locked with a PIN
} fh1_user_t;

// After storage and settings: loads the users, makes the unit's own key on first start.
void fh1_users_init(void);
const char *fh1_users_unit_id(void);

// The users, owner first, then local users, then visitors (by name).
int fh1_users_count(void);
bool fh1_users_at(int i, fh1_user_t *out);
bool fh1_users_get(const char *id, fh1_user_t *out);
const char *fh1_users_owner(void);       // "" with no users

// Names: 3 to 16 of a-z 0-9 _ -, unique here. Upper case is lowered. why gets a reason (English, for T()).
bool fh1_users_name_check(const char *name, const char *except_id, char *clean, size_t n, const char **why);

// The first user made is the owner (and takes over the unit's app data). Errors: ESP_ERR_INVALID_ARG (name),
// ESP_ERR_NO_MEM (full), ESP_FAIL (storage).
esp_err_t fh1_users_create(const char *name, fh1_user_kind_t kind, char id_out[FH1_USER_ID_LEN + 1]);
// pin: the user's PIN when they have one (else ignored). ESP_ERR_INVALID_CRC: a wrong PIN.
esp_err_t fh1_users_rename(const char *id, const char *name, const char *pin);
// The avatar code (fh1_avatar.h), re-signed like a rename.
esp_err_t fh1_users_set_avatar(const char *id, const uint8_t *code, size_t n, const char *pin);
esp_err_t fh1_users_make_owner(const char *id);       // a local user; the caller checks the owner's PIN
esp_err_t fh1_users_remove(const char *id);           // not the owner: hand ownership over first

// PINs: digits, 4 to 8. new_pin "" removes it. ESP_ERR_INVALID_CRC: old_pin is wrong.
esp_err_t fh1_users_set_pin(const char *id, const char *old_pin, const char *new_pin);
bool fh1_users_check_pin(const char *id, const char *pin);   // true also when the user has no PIN

// Controllers (by gamepad id, fh1_gamepad.h). user_id "" frees it.
esp_err_t fh1_users_link_pad(const char *pad_id, const char *user_id);
bool fh1_users_pad_user(const char *pad_id, char user_id[FH1_USER_ID_LEN + 1]);
int fh1_users_pads_of(const char *user_id, char ids[][18], int max);

// Players (0-based): the user of a player's controller. Player 1 without one: the user in use, unless they play
// as another player with their own controller; then the owner, then the first local user free.
bool fh1_users_player(int player, fh1_user_t *out);
void fh1_users_use(const char *id);      // player 1 without a controller of their own (until restart)
const char *fh1_users_in_use(void);
// A user's own data folder for an app (fh1_data_dir gives player 1's).
void fh1_users_data_dir(const char *user_id, const char *app, char *out, size_t n);
// Removes an app's data from every user (uninstalling).
void fh1_users_remove_app_data(const char *app);

// The signed public record (CBOR), and checking one from elsewhere.
size_t fh1_users_record(const char *id, uint8_t *out, size_t max);
esp_err_t fh1_users_verify_record(const uint8_t *rec, size_t n, fh1_user_t *out);

// Backups: <FH1_USERS_BACKUP_DIR>/<name>-<tag>.fh1u, locked with a passphrase (at least 6 characters).
esp_err_t fh1_users_export(const char *id, const char *pin, const char *pass, char *path_out, size_t n);
int fh1_users_backups(char names[][40], int max);     // file names in the backup folder
// Restores a backup as a local user (a newer record of an existing user updates it). ESP_ERR_INVALID_CRC: wrong
// passphrase; ESP_ERR_INVALID_STATE: already here, not newer.
esp_err_t fh1_users_import(const char *file, const char *pass, char id_out[FH1_USER_ID_LEN + 1]);

// ── Contacts (U4): other units' users seen on the network, verified, the newest rev kept ──
#define FH1_CONTACTS_MAX 32
typedef struct {
    fh1_user_t u;                       // their profile (no key; kind and owner unused)
    char unit[FH1_USER_ID_LEN + 1];     // the unit they were seen on
    uint32_t ip;                        // its address there (IPv4, network order), 0 unknown
    uint16_t port;                      // its web server's port (80 on an FH1)
    uint32_t seen;                      // when (time(), seconds; 0 when the clock was not set)
} fh1_contact_t;
int fh1_contacts_count(void);
bool fh1_contacts_at(int i, fh1_contact_t *out);
bool fh1_contacts_get(const char *id, fh1_contact_t *out);
// A profile from the network (checked here). ESP_OK when kept (new, newer, or seen again); *is_new the first
// time. A local user's own profile is not a contact (ESP_ERR_INVALID_STATE).
esp_err_t fh1_contacts_add(const uint8_t *rec, size_t n, const char *unit, uint32_t ip, uint16_t port, bool *is_new);
esp_err_t fh1_contacts_remove(const char *id);

// ── Trust (U5): statements signed by who makes them: from -> to, a level, a rev ──
// A friendship is two statements, one each way, at FRIEND or more. A request is a statement not yet answered.
enum { FH1_TRUST_NONE, FH1_TRUST_KNOWN, FH1_TRUST_FRIEND, FH1_TRUST_TRUSTED };
// What a local user says about another (out) and what the other says back (in).
void fh1_users_relation(const char *local, const char *other, uint8_t *out, uint8_t *in);
// The others a local user has statements with, either way.
int fh1_users_relations(const char *local, char ids[][FH1_USER_ID_LEN + 1], int max);
// local says level about other: signed (with local's PIN), kept, and sent (another local user gets it at once;
// a contact through the outbox, to their unit).
esp_err_t fh1_users_trust(const char *local, const char *other, uint8_t level, const char *pin);
// A message from the network: "FH1T" [1] [length][the sender's profile] [length][their statement]. Checked,
// kept for the local user it names. ip: where it came from. Out: who to, who from, the level.
esp_err_t fh1_users_receive(const uint8_t *msg, size_t n, uint32_t ip, char local[FH1_USER_ID_LEN + 1],
                            char from[FH1_USER_ID_LEN + 1], uint8_t *level);
// The outbox: statements waiting for their unit to be reached.
#define FH1_TRUST_MSG_MAX 400
typedef struct {
    char local[FH1_USER_ID_LEN + 1], other[FH1_USER_ID_LEN + 1];
    uint32_t ip;                        // the other's unit, as last seen
    uint16_t port;
    uint16_t len;
    uint8_t msg[FH1_TRUST_MSG_MAX];
} fh1_outbox_t;
int fh1_users_outbox(fh1_outbox_t *out, int max);
void fh1_users_outbox_done(const char *local, const char *other);

// ── Shared content (U5): a small header signed by the author, in front of an item ──
// { v, author key, SHA-256 of the item, size, who may see it }. Friends: the author and the viewer are
// friends both ways; trusted: the author trusts the viewer.
enum { FH1_SHARE_EVERYONE, FH1_SHARE_FRIENDS, FH1_SHARE_TRUSTED };
esp_err_t fh1_users_share_sign(const char *author, const char *pin, const uint8_t sha[32], uint32_t size, uint8_t who,
                               uint8_t *out, size_t max, size_t *n);
// Checks a header: ESP_OK when it is valid (author out); allowed: whether a local user may see the item.
esp_err_t fh1_users_share_check(const uint8_t *hdr, size_t n, const char *viewer, char author[FH1_USER_ID_LEN + 1], bool *allowed);

// Dev: back to no users; the owner's data becomes the unit's again, the others' is deleted.
esp_err_t fh1_users_reset(void);

// Called after any change (up to 4).
void fh1_users_on_change(void (*cb)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif
