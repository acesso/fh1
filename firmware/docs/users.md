# Users: identity, avatars, friends

The people on an FH1: identities, the owner, local users and visitors, PINs, backups, controllers and players,
per-user saves, avatars, notifications with faces, people nearby and friends between units, and the SDK for
apps. Code: `components/fh1_users` (identities, `users_social.c` for contacts, trust and shared-content
headers), `components/fh1_avatar` (avatars), `components/fh1_apps/apps_users.c` (the Users menu) and
`components/fh1_apps/apps_net.c` (between units). How to use it: `firmware/README.md`, "Users".

FH1 units live in a larger network (several FH1s on Wi-Fi, bridges). A user's data travels between units and
never collides, with no central server and no reliable clock.

## Identity: keys, not names

- Every user gets an **Ed25519 key pair** (Monocypher, CC0 or BSD-2) when created on a unit.
- **User id** = the first 10 bytes of SHA-256(public key), written in base32 (Crockford's alphabet: no I, L, O,
  U): 16 characters, 80 bits. Nobody hands ids out, so two units never pick the same one.
- **Short name**: what people type and see. 3 to 16 characters from `a-z 0-9 _ -`, unique **on a unit**, not
  in the network. Wherever names from other units can meet, it shows with a **tag**: the first 4 characters
  of the id, `igor#K7Q2`, and the face.
- **The profile**: a small record **signed with the user's key**:

  | Field | What |
  |---|---|
  | `v` | format version |
  | `pk` | public key (32 bytes); `id` follows from it |
  | `name` | short name |
  | `avatar` | the avatar code (below) |
  | `rev` | a counter, +1 on every change |
  | `dev` | the unit it was last changed on (unit id) |
  | `sig` | signature over all of the above |

  Canonical CBOR (a small encoder and decoder in `users.c`): a map with integer keys 0 to 6 in this order (v,
  pk, name, avatar, rev, dev, sig), shortest encodings; the signature covers the same map without key 6. A
  record that does not re-encode to the same bytes is refused. Only the key's holder can change a record, and
  the highest `rev` wins. No timestamps are needed.
- **Units have ids too**: each FH1 makes its own key at first start (unit id the same way).
- **Private keys** stay on the unit, in NVS (namespace `users`), with the profiles and the controller links, so
  formatting the internal storage keeps the identities. Optional **PIN** per user: the key is then locked with
  XChaCha20-Poly1305 under a key from Argon2id(PIN; 256 KB, 3 passes, ~130 ms).
- **Backup**: a file on the SD card, `/sd/users/<name>-<TAG>.fh1u`, locked the same way with a passphrase
  (1 MB, ~0.7 s), holding the key and the profile; restoring makes a local user (without the PIN, which
  belongs to a unit).

## Users on a unit: owner, local users, visitors

- **Owner**: administers the unit: settings, installing, Wi-Fi, other users. The first user created is the
  owner; ownership can be handed to another user on the unit. The saves made before any user existed become
  the owner's.
- **Local users**: people who use this FH1, with keys made here.
- **Visitors**: people from elsewhere playing on this FH1 for a while, never its owner. The owner can remove
  visitors; their saves on this unit go with them.
- **The Users menu** (a top-level menu next to Apps and System): the list, **New user**, **New visitor**,
  **Restore from SD**, **People nearby**, and per user: **Edit avatar**, **Play as player 1**, **Link** /
  **Free a controller**, **Friends**, **Rename**, **PIN**, **Back up to SD**, **Make owner** and **Remove**
  (both with the owner's PIN).

### Users and controllers

A controller can be a user's ("this DualSense is igor's"). The link is kept on the unit (controller id, its
Bluetooth address, to user id; NVS `users`/`pads`), set from the user's window or from the controller's own
window (Inputs and outputs: "Belongs to").

- When a linked controller connects, **its user becomes the player it plays for**. Player 1 is also the
  **interface user** (the one whose saves the menus use). With no linked controller, player 1 is the user
  picked with Play as player 1 (with their PIN if set; the owner after a restart), unless that user plays as
  another player with their own controller: then the owner, then the first local user free.
- Several users are active at once, one per player. Apps see each player's user (name, tag, face).
- Saved data per user: `/int/users/<id>/data/<app id>/` (`fh1_data_dir`: emulators and packages use it). With
  no users it is `/int/data/<app id>/`.
- Storage: NVS `users` holds the identities (`unit`, `owner`, `list`, and per user `p<id>` the profile,
  `m<id>` its kind, `k<id>` the key; `pads` the links). `/int/users/<id>/` holds the data, `trust/` and
  `outbox/`; `/int/users/contacts/` the contacts.

## Avatars

**Drawn by code from a short code.** An avatar is not a picture that travels: it is a choice of parts and
colours, and every FH1 draws it the same way. The style is anime-inspired shaded pixel art (3 tones per colour,
a coloured outline), front-facing, previewed in `avatar-showcase.html` (this folder). `components/fh1_avatar` is
a C port of that page's renderer, pixel-identical to it (checked on 432 renders: 4 characters, 12 expressions,
3 times, 3 sizes).

| View | Size (pixels) | Where | Drawing time on the S3 |
|---|---|---|---|
| Full figure | 96 x 128 | profile, editor | 15.5 ms |
| Face | 32 x 32 | notifications | 4.2 ms |
| Mini face | 16 x 16 | lists, inline | 1.1 ms |

- **The avatar code** is 12 bytes in the profile's `avatar` field: `[1][body, hair style, hair colour, skin,
  eyes, top, top colour, bottom, bottom colour, accessory colour][accessory flags]`. A value a unit does not
  know falls back to the first choice. A user without one is drawn from their id (the same on every unit).
- **Parts**: 2 bodies, 6 hair styles, 5 tops, 4 bottoms, 6 accessories (round and square glasses, earrings,
  scarf, necklace, face marks). Colours from small palettes (skin 6, hair 10, eyes 6, clothes 12, accessories 8),
  each expanded into shadow, base and light.
- **Expressions, animated like emoji**: neutral, happy, laughing, sad, crying, angry, surprised, winking,
  in love, sleepy, cool, thinking. Each swaps eyes, brows and mouth, and most add an effect (hearts, sparkles,
  a tear, an anger mark, "zzz", "?", "!", ...). Every face is animated, the mini face too: eyes blink and the
  head moves per expression.
- **Drawing**: frames are drawn as they are shown (120 ms per frame) into PSRAM and shown as images
  (`fh1_ui_pixels`). Each shape is tested once per pixel into a mask; polygons are filled by rows.
- **The editor** (Edit avatar; it opens by itself for a new user): the full figure, the face and the mini face,
  animated; the choices (Left/Right or A change the focused one), an expression to preview, Random, and Save
  (with the user's PIN; the profile is signed again).
- **In lists** a user is their animated mini face and their name, with badges over the face: a crown for the
  owner or a tag for a visitor, and their controller connected now in its colour.

## Notifications

A small box slides in at the top right for 4 s with a face (32 x 32, animated), a title and a line
(`fh1_ui_notify`); they queue, and wait while a full-screen game runs. Shown when a user's controller connects
("surya: DualSense connected"), when player 1 changes hands, when an app starts from the Apps menu (who
plays), when someone nearby is seen for the first time, and for friend requests and answers.

## People nearby

Each unit announces itself by mDNS (`_fh1._tcp`, TXT `unit=<unit id>`) and serves `GET /fh1/users` on the
console's web server: `"FH1R" [1] [unit id, 16] [count] ([length u16][profile])...`. A task (stack in PSRAM, no
flash access: storing goes through the UI task) looks every minute (Users > People nearby > Look now: at once),
fetches other units' profiles, checks them, and keeps them as **contacts** (`/int/users/contacts/`, up to 32:
the newest rev and where it was seen; the oldest seen goes when full). A contact can be forgotten.

## Friends

- **Trust statements**, signed by who makes them: `{0: v1, 1: from's key, 2: to's id, 3: level, 4: rev,
  5: signature}` (canonical CBOR, Ed25519), level `known`, `friend` or `trusted`, kept per local user in
  `/int/users/<id>/trust/<other>.out` / `.in`.
- A friendship is two statements, one each way. A **friend request** is a statement sent with the sender's
  profile; accepting is sending one back. Removing a friend is a newer statement (`rev` + 1) with level `none`.
- Sending one to another unit goes through an outbox (`/int/users/<id>/outbox/`) and `POST /fh1/trust` (the
  sender's profile and the statement; answered 202, then checked by the UI task), retried each round until
  delivered. Between two users of the same unit it is immediate.
- A user's window has **Friends**: requests to answer (A accepts), friends, requests sent, Add a friend (people
  nearby and the other users here); A on a friend or a sent request takes it back.

## Shared-content headers

A signed header `{0: v1, 1: author's key, 2: SHA-256 of the item, 3: size, 4: who, 5: signature}`, who being
everyone, friends (both ways) or trusted (the author trusts the viewer): `fh1_users_share_sign` /
`fh1_users_share_check` (a changed header fails).

## SDK 0.4 for apps

Nothing to change in an app: the system shows who plays when it starts, and its data folder already belongs to
player 1's user. For apps that want more: `fh1_user` (who plays as a player), `fh1_user_face` (their animated
face in a window), `fh1_notify` (a notification with player 1's face), `fh1_setting_get` / `_set` (short texts
by key) and `fh1_state_save` / `_load` (whole saved states, CRC-checked), all in that user's folder. Apps never
see keys. `apps/packages/hello` uses them all.

## Testing without a second FH1

`tools/fh1peer.py serve` is a pretend unit on the PC with one user, announced by mDNS, that accepts friend
requests and checks every signature with another Ed25519 implementation.
