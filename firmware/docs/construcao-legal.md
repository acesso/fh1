# Construção Legal

An exploration and building game for the FH1 (package `construcao-legal`, in
`firmware/apps/packages/construcao-legal`): the protagonist walks a large fixed world, isometric, finds a small
village on the way, and builds his own place outside it.

## Playing

- **Walking:** the D-pad walks him in eight directions; B held runs. A diagonal (two keys together) walks along the
  map's rows and columns (24 across for 14 down), so he goes straight along a road. Water blocks him; a bridge
  carries him over the river. A building blocks the middle of its cell only, leaving a lane round it, so he walks
  between two that stand side by side; trees and stones fill nearly all theirs, so a forest stays shut. He slides
  along what blocks him, and walking into a corner eases him sideways into a gap. He has a height: stairs against a
  castle wall take him up onto its walkway (a wall of one level, nothing on it), he walks along joined walls and
  over their gates, but cannot step off a wall's edge; he comes down by the stairs (each step of his within a few
  pixels of how high he is: `world_height` in `main/world.c`).
- **Building:** X switches build mode. The cell he faces is the target, outlined green where he may and red where he
  may not. The bar at the bottom has a slot per tool with its picture: road, house, blacksmith, mill, castle wall,
  tower, stairs, tree, field, remove (a red cross). Y picks the next tool; L1 / R1 on a controller pick its variant:
  a house's walls (wood or white) and roof (blue, green, red, wood) — eight —, the blacksmith's and the mill's roof,
  a wall plain, with a window or a gate, a tower's style (nine) and colour (four), the tree's kind, wheat or corn. A
  slot shows its tool's chosen variant and, with up to eight, a dot per variant; the chosen tool is framed in
  yellow, its name and variant under the slots ("Casa branca, azul", "3/8"). What it would make shows at the target,
  see-through: the ground as it would join its neighbours (its top face only), the building turned as it would
  stand, the mill with its sails; faded red where it may not go. A uses it: he swings the hammer and the change
  comes with a puff of dust. Held, A builds on (each cell once): his facing stays as it was when A went down (so the
  target keeps its place beside him), he walks along one map axis only (the first he moves on, kept to the middle of
  his row, whatever diagonal the D-pad gives), and each new cell the target reaches is built in turn: a road ahead
  of him as he walks, or a row beside him. He builds on clean grass (a tuft is cleared), never in the village; a
  building turns its door toward him (a mill its sails), and Select (or R2 / L2) turns it a quarter further each
  press (kept for the next ones); roads join their neighbours; buildings join too, each part keeping its colours and
  the parts already built keeping their joins, into any shape and size (a long house, an L, a U, a 3 x 3 block of
  parallel rows...); a smithy joins wooden houses (a mill does not join); four houses of one kind in a square, and
  nothing more joined, make one bigger house instead, and a house built on it again adds an upper storey on its
  middle; four castle walls in a square (not part of a bigger block) make a keep, and a tower built on it stands on
  its middle; stairs go on clean grass against a wall of one level, climbing toward it (the one ahead of him first);
  he builds standing on the ground; a castle grows from a 4 x 4 block of walls (its floor): walls stacked on its
  middle 2 x 2 make a keep on the floor, and a tower on the keep's middle tops it (three levels; the small one is a
  keep and its tower); the ghost of one that would join shows the pieces it changes, a little blue, the new part
  fainter; removing a piece leaves the rest joined as they were; castle walls join walls and towers beside them
  (straights, corners, T's and crosses, a square-wave seam where two meet; a wall alone stands with its face to
  him), and he walks through a gate; a gate also goes across a road where the road runs straight through it (the
  road stays under it, walls beside it must run its way); on his own wall go up to two more levels: walls (plain, or
  with windows) and a tower on top, the tower last (a wall with a tower on it is a castle, its flag waving; on a
  gate, a gatehouse), and a level on top joins only the neighbours that reach as high; fields go in rows. Remove
  cuts a tree or clears a stone anywhere outside the village, and takes back only what he built, the top level of a
  stack first (a keep's tower, a big house's storey; a gate leaves its road). A refusal says "aqui não".
- **Inside:** facing any cell of a house, a smithy, a 2 x 2 house or a keep, from any side, outside build mode
  (facing a corner: either cell beside it), "{A} entrar" shows; A goes in. Inside, the building seen from above
  (the engine's `interior.h`), laid out as it stood on screen going in: a room for each of its cells (at most 4 x 4
  round the one he went in by), in the same places, with a doorway wherever two cells join (an L of three houses:
  three rooms in an L); a 2 x 2 house and a keep are one hall. The view follows him when the floor is larger than
  the screen. He comes in by the door in the back wall of the room he faced (a hall's back wall behind it), and
  leaving puts him back where he stood. Each room is furnished by what it is, its doorways and a passage a tile
  out from its back wall kept clear: the entry room a hall (fireplace, cabinet, table and chairs, rug, chest,
  barrel; a keep's: bookshelves, pantry, carpet and desk), a smithy's cell its forge (furnace, cauldron, bench,
  barrels), the others a bedroom, a kitchen and a store by turns. Stairs in the entry room's back corners go up and
  down between floors (a 2 x 2 house with its storey and a keep with its tower have two: upstairs one room, a
  bedroom). The pictures are the medieval interior pack's (`in.*` in `art.json`).
- **The camera** follows him; what stands in front of him (a house, a tree) is cut away round him, half
  see-through. Outside build mode Select (or R1 on a controller) turns the view a quarter clockwise, L1 back, in a
  third of a second (the engine's `viewturn.h`: the old view turns away like a cube's face as the new one turns in;
  presses during a turn follow it); he keeps facing the same way in the world, and the D-pad stays the screen's.
- **Start** (or Menu) opens the menu: continue, show the frame time (and the slowest of the last 2 s) and his cell,
  leave. His place, facing and the view (`"lugar"`) and his changes (`"mundo"`: every eighth change and on leaving) are kept between
  visits.

## The world

128 x 128 cells, made the same every time (`world_make` in `main/world.c`, from a fixed seed):

- grass with tufts, forests where a smooth noise is high, a stone here and there;
- a river from north to south, winding;
- the main road from the west edge (where he starts) east through the world, a bridge where it crosses the river,
  and a road from the north down to the village;
- the village north of the main road (round cell 80, 58): a country one, an uneven oval about 21 x 17 cells. A few
  short lanes that bend (up from the main road to a little green, west and north from it, east to the north road),
  thirteen houses scattered among them in mixed colours, each turned toward the lane beside it, fruit trees
  between, ragged round fields of wheat and corn, the blacksmith by the main road and a windmill by the big wheat
  field (`village` in `main/world.c`, places from its middle). Its cells are the village's (`F_TOWN`): nobody may
  change them.

Roads and rivers pick their piece from their neighbours (north, east, south, west): ends, straights, corners,
T-junctions, crossings, and the bridge where they meet.

## Changes and saving

The player's changes are kept per cell (`edit_t`: the cell, its ground and variant, the thing and its variant, its
flags), never as image numbers, so a save survives new art (but not a new world: cells keep their place, so a
change made where the world itself has since changed stays as it was saved); up to 2048 cells. `world_set` changes a cell, redraws
the cache round it and records it; on loading, `world_apply` replays the list over the fixed world. A thing's image
comes from its kind and variant (`world_obj_image`: a house's facing, roof and walls are bits of its variant). Cells
the player built are flagged `F_BUILT`; the village's `F_TOWN` refuses every change.

## Drawing

The tileset's top face is 1.7 : 1, not 2 : 1: tiles are 48 x 28 on screen (half the art's size). A cell's anchor is
its top face's centre; the ground is drawn first, then what stands on it, back to front.

- **The view:** cells and points are kept in the world's columns and rows; `world_view` turns the view (0..3) and
  everything drawn goes through it (`to_screen` / `to_world` in `main/world.c`, `world_to_screen` for points):
  roads, rivers and walls pick their piece from their neighbours' sides turned to the screen's, a building's
  facing turns with it (a house's four pictures, the smithy's and the sails' two), a joined building works out its
  wings in screen order, and cells are drawn back to front on screen. Turning re-picks every cell and redraws the
  cache.
- **The view cache:** the world's picture round the camera, the screen plus 96 pixels each side (512 x 432, in
  PSRAM). When the camera nears its edge the kept part moves and only the strips coming into view are drawn; a
  frame copies the screen's part out of it. The whole world is never drawn at once.
- **What moves:** a mill's sails turn: 8 frames a quarter turn (four blades: a quarter turn looks like the
  start), 150 ms each, made by `fh1art.py`'s `"spin"` from the pack's sails turned in their own plane (fitted to
  the pack's 45° frame: screen x = 0.75 p, y = q ± 0.4 p), one set for each side the sails face, on the tower's
  cap. When the frame changes, `world_tick` draws again the part of the cache under each mill in it (the box all
  its frames cover), so what stands in front of a mill still hides it. Smoke rises from the blacksmith's chimney
  (`world_smoke`, over the frame): three puffs at a time, growing (the pack's puff at three sizes) and fading as
  they go up (`fart_draw_alpha`).
- **The protagonist** is drawn over that; the things in front of him that reach over him are drawn again over his
  box, and the cutaway gives back every other pixel of what was there in an ellipse round him.
- **Houses and smithies** are rendered on the PC from simple 3D models (`tools/fh1model.py`: blocks, gable and
  hip roofs, doors and windows as panels, in the pack's flat colours and dark outlines). A building of any shape is
  drawn cell by cell, back to front: each cell's piece chosen from what is round it on screen (its ridge's way:
  along its world row if it joins that way, else down its column, alone across its door's way; which of its sides
  along the ridge join; across, a parallel row whose roof meets its own in a valley, or a wing whose roof crosses
  onto it), `c<way><along><across lo><across hi>`; then a door (on the building's front cell) or windows on its
  walls seen (`dS`, `dE`, `wS`, `wE`) and a smithy's chimney (smoke from its top). A 2 x 2 house of one kind is
  drawn whole by its front cell (`big0`, `big1` with its upper storey), and so is a keep (`keep`, a tower from the
  pack on its middle). Each piece is drawn once, in reference colours; the engine swaps the walls' and the roof's
  four colours for a cell's own as it draws (`fart_map`, the palettes `pal.*` in the pack), so the pack holds one
  piece per shape, not one per colour.
- **Joined buildings:** a house's or smithy's cell keeps the sides it is joined on in its `var` (N, E, S, W, the
  mark `JOINED`, and `UPPER` for a 2 x 2 house's storey); a building is the cells joined to one another (up to 64).
  No lane is left between joined cells. Saves from before, which kept a shape and a place in it, are read as the
  same joins.
- **Castle walls and keeps** are rendered too (`fh1model.py`'s `castle_set`, the pack's castle colours): a block the
  size of a cell for each set of sides it joins on screen (`wl<mask>.<t|b>`: straights, corners, T's and crosses),
  running on into its neighbours with no line where it joins, a parapet with merlons along its outer edges on the
  top level (the levels under another have none), and where it joins to the front a seam drawn as a square wave,
  like bricks laid into each other. A gate's arch and a window's slit are drawn on its face seen (`gS`, `gE`, `lS`,
  `lE`). Walls block their whole cell; a gate lets him through. A wall's stack is in its `var` (`STACK_*` in
  `main/world.h`), its levels 25 pixels apart. Four plain walls of one level in a square that is not part of a
  bigger block at that level are a keep (`keep_at`: on the ground, or one level up on the middle of a 4 x 4 block,
  a castle's floor, the walls under it drawn as the floor's): its cells drawn each by its own piece (`kp<piece>.<ext>`: taller, its parapet
  round the outside, the seam up to a lower wall's height where one meets it), the front one with the door and the
  tower on the keep's middle; a tower on it is kept on all four cells (`world_spread`). Towers are the pack's.
- **Flags:** a tower on top of a wall or a keep has a flag waving at its top, in the tower's colour: the engine's
  `flag.h` (frames rendered by `fh1model.py`'s `flag_set` in reference colours, drawn in any two); the cache under
  each flag is drawn again as it waves, like a mill's sails.
- **The art** (`art.json`, `tools/fh1art.py` with `"smooth": true` and `"aa": true`): the tileset at half size,
  filtered (it is drawn, not pixel art); ground blocks with `"seamless"` (no outline along their top face, so the
  ground reads as one surface); the mill at 1.4x and trees at 1.3x the tile's scale; the mill is its tower, with
  its sails apart (below); the protagonist at 0.077 of his 460-pixel frames (~26 pixels), a white template
  tinted (skin, blue shirt, brown trousers), five directions drawn and three mirrored. Everything but the ground
  keeps the soft edge its scaling made: the engine blends those pixels over what is under them, so houses, trees
  and the protagonist have smooth outlines. The bar's icons (`ic.*`) are every house, blacksmith, mill and tree at
  about 26 pixels; the road and field slots show a piece of their ground through the slot.

## Development

`make -C host` builds the game on the PC; `host/host host/tests/explore.script OUTDIR` walks the world and saves
screenshots (script lines: `key` (L1, R1, L2, R2 too: a controller's shoulder buttons), `hold KEY[+KEY] FRAMES`,
`wait`, `shot`, and the game's commands `tp U V`, which puts him at a cell, `view N`, which turns the view to N,
`tool NAME` (road, house, smithy, mill, wall, tower, stair, tree, field, remove), which picks a build tool, `enter`,
`leave` and `floor N` (inside a building), `info`, and `log`, his place, his target and the changes kept);
`host/tests/build.script` builds with every tool, picks variants, and removes; `host/tests/hold.script` builds on
with A held; `host/tests/ghost.script` shows the ghost, turned; `host/tests/between.script` walks between two
houses; `host/tests/castle.script` builds walls with a corner, a gate and a tower; `host/tests/merge.script` joins
houses into a pair, an L and a square, of any colours, and removes a piece; `host/tests/stack.script` stacks walls
and towers and puts a gate across the road; `host/tests/view.script` turns the view; `host/tests/big.script` builds
a 3 x 3 block, a U, a big house with its storey and a keep with its tower; `host/tests/castle4.script` a castle of
three levels; `host/tests/stairs.script` climbs a wall and walks along it; `host/tests/interior.script` goes into a
house, an L of three houses from its side (and turns the view, a shot half way through), a 2 x 2 house, a smithy
and a keep. On the FH1 the same commands come from the console. The game prints no
floats (hundredths as whole numbers): a float through printf needs double helpers the firmware does not export, and
the package check refuses it.
