# Sound effects and a shared asset pool

## The sound pack, measured

`assets/RPG_Essentials_Free.zip`: 49 sound effects in five folders (battle, menu, movement, attack magic,
buffs and heals).

- **As delivered:** 44.1 kHz, 24-bit, stereo, each file padded with silence to a fixed length: 25.5 MB in all.
- **Real sound:** only 53.6 s in all (the part above -48 dBFS). The padding is most of every file: a footstep
  is 0.04-0.10 s of sound in a 0.67 s file.
- **Stereo:** every menu and movement sound, and most battle sounds, have identical channels; the magic and
  buff sounds differ a little between channels (6-30 dB below the sound). The FH1's speaker is mono.
- **Loudness:** peaks from -3.8 dBFS (Heal) to -22 dBFS (Jump, Step water): uneven, the steps far quieter.
- **High frequencies** (energy above 11 kHz, which a 22 kHz rate cuts):

  | Folder | Above 8 kHz | Above 11 kHz | Bright exceptions (above 11 kHz) |
  |---|---|---|---|
  | Movement | 0.3 % | 0.1 % | |
  | Battle | 3.5 % | 1.9 % | Slash 8.5 %, Bite 6.1 % |
  | Attack magic | 4.1 % | 2.3 % | Ice explosion 15.2 % |
  | Buffs and heals | 9.0 % | 1.7 % | Revive 12.2 % |
  | Menu | 9.5 % | 8.0 % | Buy_sell 79.2 % (a coin's clink) |

## What the FH1 should get

Yes: downsample, mono, trim, and a lighter codec. Per sound, from its folder and name:

| Rule | Sounds | Rate | Why |
|---|---|---|---|
| Movement | `12_Player_Movement_SFX/*` (steps, jump, landing, climb, swim, dive, teleport) | 16 kHz | no content above 8 kHz |
| Bright | names `Buy_sell`, `Ice_explosion`, `Revive`, `Slash` | 32 kHz | their sparkle is above 11 kHz |
| Everything else | battle, menu, magic, buffs | 22.05 kHz | under 3 % of the energy above 11 kHz |

For every sound:

- **Mono:** both channels mixed.
- **Trimmed:** the silence before and after removed (-48 dBFS), a 15 ms fade at the end.
- **Levelled:** peaks set to -3 dBFS, then a gain per folder so they sit together: movement -9 dB, menu -6 dB,
  buffs -3 dB, battle and magic 0 dB.
- **Codec:** IMA ADPCM, 4 bits a sample: 4 times smaller than 16-bit PCM, decoded for almost nothing (the
  firmware already has it: `fh1_bridge/include/fh1_adpcm.h`). A sound whose ADPCM version sounds worse can be
  kept as 16-bit PCM by name.

| Version | Size |
|---|---|
| As delivered | 25.5 MB |
| Trimmed only (44.1 kHz, 24-bit, stereo) | 14.2 MB |
| The rules above, 16-bit PCM | about 2.4 MB |
| The rules above, IMA ADPCM | about 0.6 MB |

**To listen before deciding:** `~/ham/sfx-ab/` holds six sounds (slash, buy, step, thunder, heal, ice), each
as `0-original-44k`, `1-pcm16-22k` / `16k`, `2-adpcm-32k` / `22k` / `16k` (trimmed and levelled already).

## A shared asset pool

Games for the engine share most of their assets (tiles, characters, icons, sounds). They should be
installed once and used by every game that needs them.

- **Asset packages:** an `.fpk` with `"kind": "assets"` and no `app.elf`, installed to `/int/assets/<id>/`.
  For example `org.fh1.assets.iso-world` (tiles, the banado characters, the critters, the bag icons, the
  swords) and `org.fh1.assets.rpg-sfx` (this sound pack).
- **Apps name what they use** in their manifest: `"assets": [{"id": "org.fh1.assets.rpg-sfx", "version": "1"}]`.
  The launcher greys out an app whose assets are missing ("needs assets ..."), as it does for `needs`. The
  installer will not remove an asset package that an installed app uses.
- **SDK 0.7:** `fh1_asset_dir(id)` returns the folder of an installed asset package (or NULL), and games load
  their packs from there. A game keeps only its own assets in its package.
- **Built from the repository:** each pool has a recipe in `assets/pools/<id>/`, naming files under
  `assets/packs/`. `fh1art.py` builds the art; a new `fh1sfx.py` builds the sound bank (`.fsfx`: a table of
  names, rates, formats and lengths, then the samples) by the rules above. `tools/pkgbuild.sh` packs it all.
- **Saving space:** the sound bank goes in once, not once per game, and the same for the art (today
  Táticas da Ilha carries 518 KB of art that a second isometric game would carry again).

## Playing sounds: a mixer in the firmware

A game's frame must not wait for audio, and several sounds overlap (a slash, the hit, a step). So the
firmware mixes, not the game:

- **SDK 0.7:** `fh1_sfx_bank(path)` loads a bank into PSRAM and returns a handle;
  `fh1_sfx_play(bank, "name", volume, pan)` plays a sound and returns a voice; `fh1_sfx_stop(voice)`;
  `fh1_sfx_volume(...)`.
- **Mixing:** a mixer task on core 0 mixes 8 voices at 22.05 kHz, decoding ADPCM and resampling 16 or 32 kHz
  sounds to its rate, and writes to `fh1_audio` (the FH1's ES8311, the dev board's PCM5102A, or a bridge's
  speaker). When all 8 are busy, the oldest voice is dropped. Everything stops when the app closes.
- **Cost:** 8 voices × 22 050 samples a second: well under 1 % of a core; one PSRAM bank of about 0.6 MB.
- Later, a game's music can be one more voice, streamed.

## Sounds in Táticas da Ilha

Events, mapped by name in the game's recipe:

| Event | Sound |
|---|---|
| Moving in menus; choosing; going back; not allowed | Hover, Confirm, Decline, Denied |
| Pausing (the battle menu); back | Pause, Unpause |
| A step (by the ground: grass, rock, water); a climb or a drop | Step_grass, Step_rock, Step_water; Jump, Landing |
| A sword or axe swing; a hit; a bite or claw; the enemy falls | Slash, Impact_flesh, Bite / Claw, Enemy_death |
| A bow shot | Attack (a whoosh), then Impact_flesh |
| Fogo, Trovão, Cura | Fire_explosion, Thunder, Heal |
| An item thrown; a revive | use_item; Revive |
| Equip, unequip; buying, selling | Equip, Unequip; Buy_sell |
| A battle begins | Encounter |

## Steps

1. **The rules, decided:** listen to `~/ham/sfx-ab/`; choose ADPCM or PCM, and check the rates.
2. **`tools/fh1sfx.py`:** the rules, the `.fsfx` bank, preview WAVs of everything it makes.
3. **Asset packages:** the `.fpk` kind, the installer, the launcher's check, `fh1_asset_dir` (SDK 0.7).
4. **The mixer and `fh1_sfx_*`** in the firmware (SDK 0.7). Dev commands to play any sound on the FH1.
5. **Táticas da Ilha:** its art moved into `org.fh1.assets.iso-world`, its sounds from
   `org.fh1.assets.rpg-sfx`. The PC host writes the mixed sound of a script to a WAV, to listen to a scene
   without the FH1.

## Decided and built (2026-11-08)

- **Format:** mono, 44.1 kHz, 16-bit PCM (the user's choice: the best quality), trimmed and levelled as above:
  `assets/pools/org.fh1.assets.rpg-sfx/` (`sfx.json`, built by `tools/fh1sfx.py`): 48 sounds, 53.4 s, 4.7 MB.
- **Where:** asset packages with `"where": "sd"` are installed to `/sd/assets/<id>/` (internal storage when there
  is no card). Packages may be up to 64 MB over the dev console's upload.
- **Loading per state:** the menus' sounds when the game starts (about 0.3 MB); a battle's sounds before it,
  behind a loading screen with a bar (`SCR_LOAD`: a battle name, "Carregando...", 48 KB read per frame); they are
  freed after the battle. The game plays silent without the asset package.
- **SDK 0.7:** `fh1_asset_dir`, `fh1_sfx_play` / `_stop` / `_stop_all` / `_playing` (`components/fh1_apps/sfx.c`:
  8 voices at 44.1 kHz, a soft limit above 75 % of full scale, on core 0; stopped before an app unloads). Apps
  name their asset packages in `"uses"`; one with a missing package does not start.
- **The PC host** records everything played into `host/out/audio.wav`.
- **Checked on the FH1:** the sound package on the SD card, the game's battle sounds loaded and played (the
  PCM5102A at 44.1 kHz, no drops, no underruns, peaks to 84 %), every scene run through, no crash.
- **Fixed on the way:** the mixer keeps a sound's position in 64 bits (32.16 in 32 bits wrapped after 65 536 frames,
  1.5 s, and a longer sound such as the battle's Encounter replayed its start forever); the mixer's task needed a 6 KB stack (the audio driver logs from inside its calls; 3 KB
  overflowed); a game closed by the system (a stop, another app starting) now frees its memory (`closed()` runs
  from the unload; a stopped game had kept 2.3 MB of PSRAM); the console's stats are always valid UTF-8 (a cut
  letter at the log's start, or a just-ended task's name, closed the PC tools' connection); `tools/pkgbuild.sh`
  runs `tools/elfcheck.py`, which fails the build when an app needs a symbol the firmware does not export (a
  source file missing from the build, a compiler helper like `__floatundisf`), instead of the FH1 refusing it.
