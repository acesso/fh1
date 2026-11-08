# Vendored code in fh1_users

Everything under `vendor/` comes from other projects; our own code is `users.c` and `include/`. The users plan
is `firmware/docs/users.md`.

| Directory | From | Version | Licence |
|---|---|---|---|
| `vendor/monocypher` | Monocypher (https://monocypher.org), `src/monocypher.{c,h}`, `src/optional/monocypher-ed25519.{c,h}` | 4.0.2, release tarball `monocypher-4.0.2.tar.gz`, SHA-256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c` (the same file from monocypher.org and from the GitHub release) | CC0 or BSD 2-clause, the user's choice (`LICENCE.md`) |

Permissive: the firmware's licence is not affected.

Used: Ed25519 (`crypto_ed25519_*`, the optional file, for signatures compatible with other Ed25519 tools),
Argon2id (`crypto_argon2`) to turn a PIN or passphrase into a key, XChaCha20-Poly1305 (`crypto_aead_lock` /
`unlock`) to lock keys and backups, `crypto_wipe`. SHA-256 (user ids) comes from the ESP-IDF's PSA crypto.

## FH1 changes to vendored files

None.
