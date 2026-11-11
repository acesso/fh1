# Táticas da Ilha: realms, progression and rules

The game in `apps/packages/isotest` (the FH1 game engine's test package). Every text in the game is in
Portuguese; this page uses the game's own words for its places and things. The numbers live in
`apps/packages/isotest/main/data.c`.

## The story

Four Elementals kept the island in balance: Ignis (Fogo), Maris (Água), Gaia (Terra) and Zéfiro (Ar). A shadow
fell on their realms; the Elementals went mad, and the creatures took on their masters' colours. The party
crosses each realm's fases and faces its Elemental. The title screen's "História" tells it in three pages.

## Realms, fases and battles

| Realm | Element | Ground | Creatures | Fase 1 boss | Elemental |
|---|---|---|---|---|---|
| Fogo | fire | rocks, lava for water, scorched colours | Lobo, Javali, Slime | Lobo Alfa | Ignis |
| Água | water | meadow, smaller islands (more sea), cold blue | Slime, Texugo, Lobo | Javali Rei | Maris |
| Terra | earth | forest, deeper green | Texugo, Cervo, Javali, Slime | Cervo Rei | Gaia |
| Ar | air | rocks, taller hills, pale colours | Cervo, Lobo, Slime | Lobo do Vento | Zéfiro |

- **Fases:** each realm has three, of 3, 6 and 9 battles in a row. The last battle of a fase is its boss: the
  realm's alpha creature (fase 1), the Rei Slime (fase 2), the Elemental (fase 3). Bosses are drawn at twice the
  size and fight in arenas drawn by hand (the den, the king's pool, the shrine); the other battles are islands
  made from a seed (the same island every time).
- **Opening:** a fase's first battle is open; winning a battle opens the next. Clearing a fase opens the realm's
  next fase. Realms are independent: any realm can be chosen at any time, and any battle already won (in any
  open fase) can be fought again.
- **Creatures' levels** rise through the fases: fase 1 from level 1 to 4, fase 2 from 5 to 9, fase 3 from 10 to
  15 (the boss at the top of its fase; in the bigger packs some creatures are a level below). A realm's creatures
  are its own (none cross into another realm); the critters (Scrabling's wolf, boar, badger, stag) are recoloured
  to the realm, the slimes are their own colours (the fire ones made red from the purple pack), and an Elemental
  glows in its element.

## Heroes

| Class | PV (level 1, per level) | PM | Moves, climbs | Weapons | Armour | Magic |
|---|---|---|---|---|---|---|
| Paladino | 24, +5 | 6, +1 | 4, 1 | swords | heavy | Cura |
| Ninja | 20, +4 | 0 | 5, 3 | blades | light | |
| Arqueiro | 18, +4 | 0 | 4, 2 | bows (range 4, then 5) | light | |
| Mago | 15, +3 | 14, +3 | 4, 1 | staffs | robes (+PM) | Fogo, Onda, Rocha, Trovão, Cura |

- **Levels** 1 to 15. XP to the next level: 20 + 14 × level. Every hero in the party learns from every creature
  knocked out: its XP (6 + 3 × its level; bosses 4×, 6×, 10×), a quarter less for each level the hero is above
  it (down to a quarter); half after a defeat. One realm's three fases bring a party to about level 4, 10 and 15;
  a second realm's first fase adds little.
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

Title (Começar, História, Sair) → Equipe (the party; Loja, the shop, from there) → Reinos (a card per realm: its slime, each fase's
progress, its Elemental; the chosen realm's story) → the realm's path (its fases as tabs, Up/Down; its battles
along a path, Left/Right; the chosen battle's creatures by kind and level and what it may pay) → loading (the
battle's sounds) → the battle → the summary (each creature with its coins and XP, the coins counting up, each
hero's XP bar; a hero that levels up cheers, and a new gear level is announced; a cleared fase says what the shop
opened) → the party.

In battle, a unit knocked out plays its own death (wolves, slimes) or falls over (heroes, the boar, badger and
stag), once, and lies still, its colour draining to a dim grey in under a second (back on a revive).

- **Heroes as equipped:** on the title the roster walks in place and turns, in its armour and with its weapons;
  on the party screen the chosen card's hero does, the others stand.
- **The shop** is on the party screen (Loja, beside Pronto); a battle's end goes back to the party.
- **Turning the map:** Select (or R1) spins the battlefield a quarter round the cursor, L1 the other way;
  "girar" shows in the corner while choosing. The cursor stays where it is on screen and the D-pad keeps moving
  it as on screen.
- **Water** ripples, a wave rolling across the sea; in the fire realm the lava does.
- **Bosses arrive:** in a boss battle with minions, the boss is off the field until they are all down. Then the
  ground rumbles ("Algo se aproxima..."), its shadow grows on its cell (or the nearest free one), and it comes: the
  Alpha drops from the sky and lands with a shake, the ground cracking round it (looks only); the King Slime lands
  and bounces once; an Elemental descends slowly in a swirl of its element and lands in a flash. Its name shows,
  and the battle goes on. Its card says "vindo" meanwhile.
- **Spells grow with their caster:** tier 1 up to level 4, tier 2 up to 9, tier 3 from 10. Fogo: bigger bursts,
  then flames rising and the air waving with heat, then a shake. Onda: more splash and drops, then the screen
  waving. Rocha: a rock drops onto the target and shatters, then more round it with the ground cracking and
  shaking. Trovão: the bolt, then a negative flash, then a shake. Cura: more sparkles.
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

## Development commands

`dev_command` in `isotest.c`, from the FH1's console (`op dev "..."`) or a PC host script.

- **Anywhere:** `nosave` (the saved game stays as it was until the game closes), `info` (the frame time on
  screen), `go REALM FASE BATTLE` (straight to its loading screen; realms 0 Fogo, 1 Água, 2 Terra, 3 Ar; fases and
  battles from 0), `test REALM` (the test field in a realm's colours); a battle under way is left.
- **Outside a battle:** `won REALM FASE N`, `level HERO N`, `coins N`, `give GOODS N`,
  `screen title|party|realms|path|shop`.
- **In a battle:** `place`, `act`, `counter`, `target`, `gear`, `ranged`, `cam`, `xp`, `hp`, `win`, `lose`, `log`;
  `turn 1|-1` (as Select does), `view 0..3` (at once), `turnstyle 0|1` (spinning, narrowing); `obj C R TILE FX`,
  `height C R H FX`, `top C R TILE FX` (the scenery changing: TILE a tile's number, -1 none; FX 0 at once, 1
  breaking, 2 dropping, 3 rising, 4 fading, 5 teleporting), `float C R BASE BOB`, `crack C R POWER`,
  `dropon C R TILE` (looks only); `cast EL TIER C R` (a spell's looks at a cell; EL 0 healing, 1 fire, 2 water,
  3 earth, 4 air), `tier N` (every spell at a tier; 0: by level), `screen KIND MS STRENGTH` (0 negative, 1 wave,
  2 shake); `kill UNIT` (-1: every creature but the bosses; a waiting boss then comes), `boss` (a waiting boss
  called in now); `relayer` (the layer built again from scratch), `checklayer` (the layer against a fresh build:
  "check: layer ok"), `bench` (what turns, the ripple and scenery changes cost, to the log).

**The test field** (`battle_test`, "Campo de Testes"): 14 × 14, heights 0 to 6 (a stair up to a tower), a channel
of water, a yard, rocks, plants and stumps, a still floating slab and two groups of bobbing ones; three heroes,
two of the realm's creatures and its Alpha waiting off the field.

**The suite:** `host/suite.sh [--device] [GROUP...]` runs the scripts in `host/tests/` (views, water, scenery,
float, effects, spells, overlays, boss, colors, stress) and the older ones (regress), in sets (quick, the default;
engine, game, all), and fails on writes outside the frame, commands not taken, layers that differ from a full
build, checks that never reported, and crashes. Screenshots and a contact sheet per group: `host/out/suite/GROUP/`.
`--device` runs them on the FH1 (`FH1_CODE`).

**Older host scripts:** `host/realms.script` (the screens and a battle per realm), `host/effects.script`
(elemental weapons, falling over), `host/elemental.script` (an Elemental's aura and blast), `host/resist.script`
(a fire bite on plain and fire plate, the shop after the fire fase, a hero's attributes), `host/v08.script`
(heroes in their gear, the controller styles' icons (host commands `pad fh1|xbox|nintendo|ps`), Loja, the magic
list, Esperar, the elemental arrows, a caster, another user taking player 1 over (`user NAME`)). Any script runs on
the FH1 through `host/device.py`.
