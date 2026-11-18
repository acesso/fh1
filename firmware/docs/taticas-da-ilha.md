# Táticas da Ilha: realms, progression and rules

The game in `apps/packages/taticas-da-ilha` (id `taticas-da-ilha`, package `taticas-da-ilha_<version>.fpk`; it began as the FH1 game engine's test package). Every text in the game is in
Portuguese; this page uses the game's own words for its places and things. The numbers live in
`apps/packages/taticas-da-ilha/main/data.c`.

## The story

Four Elementals kept the island in balance: Ignis (Fogo), Maris (Água), Gaia (Terra) and Zéfiro (Ar). A shadow
fell on their realms; the Elementals went mad, and the creatures took on their masters' colours. The party
crosses each realm's fases and faces its Elemental. The title screen's "História" tells it in three pages.

## Realms, fases and battles

| Realm | Element | Ground | Creatures | Fase 2 spirit | Elemental |
|---|---|---|---|---|---|
| Fogo | fire | rocks, lava for water, scorched colours; lava pits | Lobo, Javali, Slime, Rubylit, Golem de Fogo | Ifrit | Ignis |
| Água | water | meadow, smaller islands (more sea), cold blue; lakes | Slime, Texugo, Lobo, Sapphilit, Titã de Gelo | Ondina | Maris |
| Terra | earth | forest, deeper green; rocky hills | Texugo, Cervo, Javali, Slime, Esmeralit, Golem | Árvore Ancestral | Gaia |
| Ar | air | rocks, taller hills, pale colours; chasms | Cervo, Lobo, Slime, Topalit, Titã do Vento | Sílfide | Zéfiro |

- **Fases:** each realm has three, of 3, 6 and 9 battles in a row. The last battle of a fase is its boss: the
  Rei Slime (fase 1, "Lago do Rei Slime"), the realm's spirit (fase 2, "Covil: NAME"), the Elemental (fase 3).
  The king and the Elemental are drawn at twice the size (a spirit at its own: big already); bosses fight in arenas
  drawn by hand (the king's lake, the den, the shrine); the other battles are islands made from a seed (the same
  island every time).
- **Two moments** from fase 2: a battle begins on a small island, as in fase 1 (11 or 12 × 12; a boss battle in
  its arena, the boss's guards only). Once its creatures are down the island grows ("A ilha cresce!": the ground
  shaking, the new land rising from the sea in waves) to its fase's size, 24 × 24 in fase 2, 36 × 36 in fase 3,
  and the second wave teleports in ("Reforços!"): three creatures in fase 2, four in fase 3 (a boss battle: the
  boss and its others), one by one, each in a circle of the realm's element at the new land's far side, as far as
  the heroes can walk from where they stand. The camera follows.
- **Realm ground** on the big islands: a few patches the heroes cannot cross, away from the middle: rocky hills
  (Terra), lakes (Água: ice in a cold realm), lava pits (Fogo), winding chasms down into the void (Ar).
- **The party:** three heroes at first; four once every realm's first fase is cleared. Fase 1's battles take the
  first three of the party (in the roster's order); fases 2 and 3, all four.
- **The island growing** (when the last realm's fase 1 is cleared, every other one already cleared): after the
  summary the battle stays. The
  arena's island rises out of the sea to 24 × 24, its new land in waves from the old shore outward, splashing, the
  ground shaking ("A ilha desperta..."); a violet portal opens on the new land, eleven steps from the heroes, and
  a shadow steps out of it (a hero's body all in dark violet, red glints at its eyes: "Uma sombra surge...", "A
  ilha é minha!"); rune circles of the realm's element round it, and four creatures of the realm's second fase
  appear on them ("Acabem com eles!"); a golden portal opens by the heroes, the hero left out of the party steps
  out of it ("NAME: Segurem-se!"), circles of light under each hero, and they vanish into it one by one, the
  rescuer last; the shadow: "Fujam, por ora...". Dark, then the party screen with all four in the party. A (or
  Start) skips to the dark.
- **Opening:** a fase's first battle is open; winning a battle opens the next. Fase 2 opens once every realm's
  fase 1 is cleared; fase 3 once the realm's own fase 2 is. Realms are independent: any realm can be chosen at any time, and any battle already won (in any
  open fase) can be fought again.
- **Creatures' levels** rise through the fases: fase 1 from level 1 to 4, fase 2 from 5 to 9, fase 3 from 10 to
  15 (the boss at the top of its fase; in the bigger packs some creatures are a level below). A realm's creatures
  are its own (none cross into another realm); the critters (Scrabling's wolf, boar, badger, stag) are recoloured
  to the realm, the slimes are their own colours (the fire ones made red from the purple pack), and an Elemental
  glows in its element. Each realm has a crystal spirit of its colour (Rubylit, Sapphilit, Esmeralit: Sapphilit
  made green, Topalit; a little sturdier than a slime, its crystal shatters when it falls) and, from fase 2, a big
  one, slow and hard to break (Golem de Fogo, Titã de Gelo, Golem, Titã do Vento: the Ice Titan in pale gold).
  The fase 2 spirits: Ifrit, Ondina (the Aqua Elemental in blue), Árvore Ancestral (the Ancient Tree), Sílfide (the
  Aqua Elemental in pale gold, its walk a whirl). A realm's own creatures are a pack of their own
  (`assets/realm-KEY.fart`), added to the art while one of its battles is on.

## Heroes

| Class | PV (level 1, per level) | PM | Moves, climbs | Weapons | Armour | Magic |
|---|---|---|---|---|---|---|
| Paladino | 24, +5 | 6, +1 | 4, 1 | swords | heavy | Cura |
| Ninja | 20, +4 | 0 | 5, 3 | blades | light | |
| Arqueiro | 18, +4 | 0 | 4, 2 | bows (range 4, then 5) | light | |
| Mago | 15, +3 | 14, +3 | 4, 1 | staffs | robes (+PM) | Fogo, Onda, Rocha, Trovão, Cura |

- **Levels** 1 to 15. XP to the next level: 20 + 14 × level. Every hero standing at the end learns from every
  creature knocked out: its XP (6 + 3 × its level; bosses 4×, 6×, 10×), a quarter less for each level the hero
  is above it (down to a quarter). A hero knocked out learns nothing; its share goes to the heroes standing (each
  gets the party's size over how many stand, times its own: two of three standing get 1.5×). Leaving a battle,
  half; a defeat (every hero down), nothing. One realm's three fases bring a party to about level 4, 10 and 15;
  a second realm's first fase adds little.
- **Guarda** (from level 5): the hero kneels and holds its ground, its turn over; until its next turn every blow
  on it does half (rounded up). Its menu shows "Nv5" below level 5.
- **Gear levels:** every 5 levels a hero can use the next level of its class's gear: level 0 from the start,
  level 1 from hero level 5, level 2 from 10, level 3 from 15. The party's and the battle's equip lists show the
  class's gear only, greyed with the level it needs.

## Damage

- **A hero's attack:** its level, plus its weapon's roll (level n: n × 2 + 1 to n × 2 + 2; level 0 is 1 to 2,
  level 1 is 3 to 4), less the target's defence; at least 1. The hero's level matters most.
- **Elements:** an elemental weapon's roll counts twice against a creature of the element it beats. Fire beats
  water, water beats earth, earth beats air, air beats fire (a fire Gládio, 3 to 4, does 6 to 8 more against
  Água's creatures). "Fraco!" shows over the target.
- **Spells:** magic's level is the weapon's (a staff's). A spell adds the staff's roll + 1, twice when the staff
  is of the spell's element; then, on a target, twice against the element the spell beats ("Fraco!") and half
  against its own ("Resiste"). Cura heals 8 + 2 × level + that addition. The magic list shows each spell in its
  element's colour, its PM and its addition ("4PM +3~4", gold when the staff doubles it), titled with the staff's
  level ("Magia Nv 1"). A weapon's roll is halved the same way against its own element.
- **Creatures:** their level + 2 + their kind's bonus, to +1, less the hero's armour; the Elemental strikes from
  3 cells away with its element. Every creature's attack is of its realm's element.
- **Casters:** from fase 2 on, one creature in each pack (in a boss's arena, one of its guards) is a caster ("Lobo
  Arcano", "mago" on its card, its element flickering over it): it casts its realm's element from 4 cells away,
  staying put when a hero is in reach (a little weaker, four fifths of the PV, worth half as much again).
- **Elements in flight:** a creature's cast or an archer's arrow of an elemental bow: fire, a fireball or a burning
  arrow; water, an ice needle that shatters; earth, a rock spike bursting from the ground under the target; air, a
  lightning strike (casters) or the tempest arrow (wind wound round it in two spirals, white after-images, speed
  lines) that ends in a whirlwind rising round the target, crackling.
- **Elemental armour** halves the damage of its own element ("Resiste" over the hero).
- **The target's card** shows the range ("5~6"), "cai" when it surely falls, "cai?" when it may.

## Gear and the shop

| Level | Swords (Paladino) | Blades (Ninja) | Bows (Arqueiro) | Staffs (Mago) | Price |
|---|---|---|---|---|---|
| 0 | Espada | Adaga | Arco | Cajado | 40 |
| 1 | Gládio | Katana | Arco Longo | Bastão | 220 |
| 2 | Montante | Kodachi | Arco Duplo | Cetro | 600 |
| 3 | Espadão | Nodachi | Arco Real | Báculo | 1400 |

| Level | Heavy (Paladino) | Light (Ninja, Arqueiro) | Robes (Mago) | Price |
|---|---|---|---|---|
| 0 | Cota de Malha, defence 2 | Couro, 1 | Manto, 0, PM +5 | 30 |
| 1 | Placas, 4 | Couro Batido, 3 | Manto Azul, 1, PM +10 | 180 |
| 2 | Placas de Ouro, 6 | Gibão Élfico, 5 | Manto Rúnico, 3, PM +15 | 500 |
| 3 | Arm. Sagrada, 8 | Escamas, 7 | Manto Arcano, 4, PM +20 | 1200 |

- **Elemental weapons:** each weapon of levels 1 to 3 in each element ("Gládio de Fogo", "Arco Longo da Terra"),
  at one and a half times the price. They are drawn in their element's colour, spark while held and along a
  swing, leave a trail behind their arrows, and burst in their element on a hit.
- **Elemental armour:** each armour family of levels 1 to 3 in each element: Placas, Couraça, Armadura (heavy);
  Couro, Gibão, Escamas (light); Manto, Túnica, Veste (robes), "de Fogo", "de Água", "da Terra", "do Ar". The
  plain armour's defence and PM of the same level, half the damage of its element, one and a half times the price,
  drawn in its element's colours.
- **Items:** Poção (PV +25), Poção Forte (PV +60), Éter (PM +15), Elixir (all PV and PM, brings a fallen hero
  back).
- **In the shop:** level 0 always; level n once any realm's fase n is cleared; elemental weapons and armour of
  level n once their own realm's fase n is cleared. Nothing that does nothing is sold. Selling pays half.
- **Coins** per creature: 4 + 2 × its level (bosses times their XP factor); half after a defeat.

## Screens

Title (Continuar, a saved game's; Novo jogo, asking "Começar de novo? O jogo salvo será apagado." first when
there is one; História; Sair) → Equipe (the party; Loja, the shop, from there) → Reinos (a card per realm: its slime, each fase's
progress, its Elemental; the chosen realm's story) → the realm's path (its fases as tabs, Up/Down; its battles
along a path, Left/Right; the chosen battle's creatures by kind and level and what it may pay) → loading (the
battle's sounds) → the battle → the summary (each creature with its coins and XP, standing through its idle once
and then dying, in its colours; the coins counting up; each hero's XP bar; a hero that levels up cheers twice, and
a new gear level is announced; a hero knocked out stands, then kneels and shows "sem XP"; a cleared fase says what
the shop opened; with four heroes their rows closer, a level's gains on the name's line; with more than six
creatures, no XP under each and their coins staggered) → the party (after the last realm's first fase cleared: the
island growing, then the party).

In battle, a unit knocked out plays its own death (wolves, slimes) or falls over (heroes, the boar, badger and
stag), once, and lies still, its colour draining to a dim grey in under a second (back on a revive).

- **Heroes as equipped:** on the title the roster walks in place and turns through eight facings, in its armour
  and with its weapons; on the party screen the chosen card's hero does, the others stand.
- **Eight facings:** the heroes turn an eighth at a time, turn with the map when it spins, and face a target off
  the map's lines (an arrow or a spell sent askew).
- **The shop** is on the party screen (Loja, beside Pronto); a battle's end goes back to the party.
- **The heroes' showcase** (a secret: the Konami code on the title, Up Up Down Down Left Right Left Right B A):
  a random realm and map with a flat stage, the four heroes side by side facing four of its creatures. In turn,
  each hero steps two cells up to its creature and shows every skill it has (its attack, then each spell its class
  knows; the creatures take no damage, but every effect and number plays), takes one hit and kneels, is revived
  with an Elixir by the hero beside it, cheers and steps back into line. Then "Vitória!" and back to the title. No
  save, no summary; the menu (B, Start) continues, goes back to the title or quits. The stage has the portal pack's
  props: a portal behind the creatures between two broken pillars and a campfire past each end of the heroes'
  row. The portal is the engine's (`portal.h`): its frame is scenery (`prop.portal.frame`), its vortex whirls in
  the realm's colour (its light on the frame too), opens from its middle as the show starts and closes into it at
  the end, a few motes of light drifting round it; the map engine's `on_scenery` hook draws it over the layer,
  under the units. The campfire's flames flicker, sparks rise (`fx_aura`'s rising kind) and a warm light flickers
  on the ground round it (`fx_light`); fire gives off light, so the realm's grade leaves it in its own colours
  (`iso_lit`). The props are in the pack for any battle (`prop.*`, `fire.flicker`, `portal.whirl`).
- **Seen through the scenery:** a hero or creature behind a taller column, a rock or a tree shows through it,
  the cover half see-through round it.
- **Moving:** choosing where to move, the cursor keeps to the cells the hero can reach; a press toward a cell it
  cannot reach jumps on to the next one it can that way.
- **Turning the map:** Select (or R1) spins the battlefield a quarter round the cursor, L1 the other way;
  "girar" shows in the corner while choosing. The cursor stays where it is on screen and the D-pad keeps moving
  it as on screen.
- **Water** by its depth: next to land it is shallow and light, a swell rolling along the shore and now and then
  a rock standing in it; further out it is deep and dark, glinting here and there. In the fire realm it is lava.
- **Bosses arrive:** in a boss battle with minions, the boss is off the field until they are all down. Then the
  ground rumbles ("Algo se aproxima..."), its shadow grows on its cell (or the nearest free one), and it comes: the
  Alpha drops from the sky and lands with a shake, the ground cracking round it (looks only); the Rei Slime lands
  and bounces once; an Elemental descends slowly in a swirl of its element and lands in a flash. Its name shows,
  and the battle goes on. Its card says "vindo" meanwhile.
- **Spells grow with their caster:** tier 1 up to level 4, tier 2 up to 9, tier 3 from 10. Fogo: bigger bursts,
  then flames rising and the air waving with heat, then a shake. Onda: more splash and drops, then the screen
  waving. Rocha: a rock drops onto the target and shatters, then more round it with the ground cracking and
  shaking. Trovão: the bolt, then a negative flash, then a shake. Cura: more sparkles.
- **Effects on every blow:** a sword or dagger leaves a slash the way it swings, an arrow bursts in a star, a
  creature's bite leaves claw marks. A caster stands in a rune circle of the spell's element, then the spell plays
  on its target: a flame pillar (Fogo), an ice burst (Onda), rubble as the rock lands (Rocha), purple lightning
  (Trovão), a lotus growing (Cura and the items).
- **Creatures stand out from their ground:** each realm tints its creatures with its element apart from its
  scenery: fire charcoal to flame gold over red rock, water navy to ice, earth umber to sandstone over grass and
  dirt, air ochre to pale gold.
- **Menus:** the chosen line waves, a letter at a time, once a second (its note on the right stays still). In a
  hero's menu the west face button (Square, X on an Xbox pad, Y on the FH1) goes straight to Esperar.
- **Button icons** in the hints are player 1's controller's (SDK 0.8's `fh1_px_key`), and change with it.
- **Per user:** the progress is player 1's user's. When another user takes player 1's controller over, the game
  loads theirs and goes to the party screen ("Progresso de Avi"); a battle under way is left unsaved.

## The save

`fh1_state_save("progresso")`, version 5: the roster (name, class, level, XP, weapon, armour, in the party), the
bag (a count per goods, up to 128), the coins, the battles won in each realm's fases. A version 4 save (a bag of
96) is carried over as it is. A version 3 save (before the realms)
is upgraded when loaded: names, classes, levels (up to 15), the party, the coins and the potions are kept; gear
goes back to each class's level 0, and the old weapons and armour in the bag are sold at half price.

## Adding a creature

From a sheet in the usual rip layout (`assets/packs/ragnarok-enemies/`): check it with `tools/fh1art.py creature
SHEET` (eight bands: idle, run and bite front and back, hurt, extras), add `{"name": "NAME", "sheet": ..., "death":
[...], "pack": "KEY"}` to `art.json`'s `"creatures"`. From a sheet laid out any other way (a boss's): `tools/fh1art.py
frames SHEET OUT.png` numbers its frames (`--split Y` parts two rows something crosses), then `"frames": {"idle.sw":
"0-5", "idle.ne": ..., "run.sw": "idle.sw" (one that floats), "bite...", "hit...", "death..."}` names them; `"scale"`
sizes it, `"recolor"` ports it to another realm's colour (`"min_sat"` low to tint pale art). `"pack"`: its realm's
pack (`assets/realm-KEY.fart`). Then in `main/`: a kind in `game.h`'s enum (before `E_COUNT`, so saved
battles keep theirs), its line in `data.c`'s `g_enemies` (name, plural, art name, stats), and the kind in its
realms' creature lists. Similar sprites in other colours (a creature's elemental kin) are each their own sheet and
kind.

## Development commands

`dev_command` in `taticas.c`, from the FH1's console (`op dev "..."`) or a PC host script.

- **Anywhere:** `nosave` (the saved game stays as it was until the game closes), `info` (the frame time on
  screen), `go REALM FASE BATTLE` (straight to its loading screen; realms 0 Fogo, 1 Água, 2 Terra, 3 Ar; fases and
  battles from 0), `test REALM` (the test field in a realm's colours), `show` (the heroes' showcase); a battle under way is
  left.
- **Outside a battle:** `won REALM FASE N`, `level HERO N`, `inparty HERO 0|1`, `coins N`, `give GOODS N`,
  `screen title|party|realms|path|shop`.
- **In a battle:** `place`, `act`, `counter`, `guard HERO` (holds its ground), `target`, `gear`, `ranged`, `cam`, `xp`, `hp`, `win`, `lose`, `log`;
  `turn 1|-1` (as Select does), `view 0..3` (at once), `turnstyle 0|1` (spinning, narrowing), `cutaway 0|1`; `obj C
  R TILE FX`, `height C R H FX`, `top C R TILE FX` (the scenery changing: TILE a tile's number, -1 none; FX 0 at
  once, 1 breaking, 2 dropping, 3 rising, 4 fading, 5 teleporting), `float C R BASE BOB`, `crack C R POWER`,
  `dropon C R TILE` (looks only); `cast EL TIER C R` (a spell's looks at a cell; EL 0 healing, 1 fire, 2 water, 3
  earth, 4 air), `tier N` (every spell at a tier; 0: by level), `screen KIND MS STRENGTH` (0 negative, 1 wave, 2
  shake); `kill UNIT` (-1: every creature on the field but the bosses; a waiting boss then comes; with a
  second wave still to come, the next turn's end brings it), `boss` (a waiting boss called
  in now); `relayer` (the layer built again from scratch), `checklayer` (the layer against a fresh build: "check:
  layer ok"), `bench` (what turns, the ripple and scenery changes cost, to the log).

**The test field** (`battle_test`, "Campo de Testes"): 14 × 14, heights 0 to 6 (a stair up to a tower), a channel
of water, a yard, rocks, plants and stumps, a still floating slab and two groups of bobbing ones; three heroes,
two of the realm's creatures and its Alpha waiting off the field.

**The suite:** `host/suite.sh [--device] [GROUP...]` runs the scripts in `host/tests/` (views, water, scenery,
float, facing, cutaway, effects, fx, spells, overlays, boss, colors, summary, party4, show, grow, stress) and the older ones (regress), in sets (quick, the default;
engine, game, all), and fails on writes outside the frame, commands not taken, layers that differ from a full
build, checks that never reported, and crashes. Screenshots and a contact sheet per group: `host/out/suite/GROUP/`.
`--device` runs them on the FH1 (`FH1_CODE`).

**Older host scripts:** `host/realms.script` (the screens and a battle per realm), `host/effects.script`
(elemental weapons, falling over), `host/elemental.script` (an Elemental's aura and blast), `host/resist.script`
(a fire bite on plain and fire plate, the shop after the fire fase, a hero's attributes), `host/v08.script`
(heroes in their gear, the controller styles' icons (host commands `pad fh1|xbox|nintendo|ps`), Loja, the magic
list, Esperar, the elemental arrows, a caster, another user taking player 1 over (`user NAME`)). Any script runs on
the FH1 through `host/device.py`.
