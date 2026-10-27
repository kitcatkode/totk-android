# Bottom screen: Map and Inventory tabs (research, 2026-10-23)

The second screen (runtime/bottom_screen.c) has a working Settings tab; Map and Inventory are placeholders. This is
what it takes to fill them, where the data lives, and a plan in steps that each end in something visible on the Thor.
Everything here is read-only: the tabs show the game's state, they do not change it (no item use, no warping).

## What the tabs need

| | live data | static assets (game files) |
| --- | --- | --- |
| Map | Link's position and heading; which layer (Sky / Surface / Depths); later: map stamps, revealed regions | the map imagery for the three layers |
| Inventory | the pouches: weapons, bows, shields, armor, arrows, materials, food, Zonai devices, key items (+ counts, durability, modifiers) | item icons, item display names |

## 1. Link's position: already solved in our runtime

`runtime/world_survey.c` (`player_actor()`) finds the player read-only: main module + `SCENE_SLOT` -> scene ->
component 13 (the resident actors) -> the link whose actor's name is `"Player"`, then `ACTOR_POS` (and `ACTOR_ROT`,
the rotation, for the heading) - offsets from aquacluck's totk-lotuskit, already ported to 1.4.3 in
`runtime/game_version.h`. Reads go through `host_safe_read`, so a stale pointer during a load costs nothing.

To do: factor a read-only `runtime_player_pose(float xyz[3], float *yaw)` out of world_survey.c that does not need
`TOTK_SURVEY` (the survey's gating protects its *write* path, forceSetMatrix; a pose read is harmless). Cache the actor
pointer, re-resolve it when the read fails or the name stops being "Player". Poll at ~10 Hz from the bottom screen.

World coordinates: x east (about -6000..6000), z south (about -5000..5000), y up. Measured heights: ground ~0-400
(Hyrule Field ~150-350), sky islands ~1500-1700+, Depths below 0. First version: layer from height (Depths if y < -100,
Sky if y > 1000) with a tap to override; later the game's own layer from GameData if one is found.

## 2. Map imagery: in the game files, as TexToGo textures (to confirm on the Thor)

The game ships its map as tiles: a full TOTK map rebuilt from the unscaled tiles is 36000x30000 per layer (Reddit
"TOTK/BOTW full resolution maps", "ripped directly from the games"). TOTK keeps most textures in `romfs/TexToGo/*.txtg`.

**TexToGo (.txtg) format** (from Switch-Toolbox's `File_Format_Library/FileFormats/Texture/TXTG.cs`, GPL - read as
documentation, do not copy code):
- header 0x50 bytes: `u16 headerSize (0x50)`, `u16 version (0x11)`, magic `"6PK0"`, `u16 width`, `u16 height`,
  `u16 depth/arrays`, `u8 mipCount`, `u8 ?`, `u8 ?`, `u16 pad`, `u8 formatFlag`, `u32 formatSetting`,
  `u8 compSel[4]` (0..3 RGBA, 4 zero, 5 one), `u8 hash[32]`, `u16 format`, `u16 (0x300)`, `u32 setting1..4`;
- at headerSize: per surface (mips x arrays) `u16 arrayLevel, u8 mipLevel, u8 1`; then per surface `u32 size, u32 6`;
- then the surfaces in that order, each **plain zstd**, each **Tegra block-linear swizzled** (default block height);
- format: 0x101 ASTC 4x4, 0x102 ASTC 8x8, 0x105 ASTC 8x8 sRGB, 0x109 ASTC 4x4 sRGB, 0x202/0x302 BC1, 0x203 BC1 sRGB,
  0x505 BC3 sRGB, 0x602/0x606/0x607 BC4, 0x702/0x703/0x707 BC5, 0x901 BC7; terrain uses 0x101 with `setting2` 32628
  meaning ASTC 8x5 and 32631 ASTC 8x8.

We already have the deswizzler (`third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c`) and BC / ASTC decoders
(`nvn_bcdec.c`, `nvn_bc1astc.c`) and in-process RomFS reads (`runtime_romfs_open` / `runtime_romfs_read`). Missing: a
zstd decoder - vendor zstd's decompress-only single file (`zstddeclib.c`, BSD) under third_party/.

**First step, on the device:** find the exact files. `TOTK_FS_LOG=1` in totk.env logs every file the game opens
(`[totk-fs] <seconds> <what> <path>`, romfs.c `fs_log`). Start the game, open the in-game map, zoom and pan over all
three layers, open the inventory and scroll each pouch, then:
`sh ~/totkdump/TOTK_android/install_app.sh log | grep totk-fs | grep -iv "Sound\|Event" > map_inventory_files.txt`.
Expect map tiles under a Map/MapTex-like folder (txtg or bntx inside a .sarc/.pack.zs), item icons either as txtg or
inside the UI layout archives (`UI/LayoutArchive/*.sarc.zs`, bntx).

**Then, a one-time conversion on the device** (a background thread on first use, cache in the game folder, e.g.
`bottom-cache/map/<layer>/<z>/<x>_<y>.rgb565`): read each tile with the RomFS API, zstd, deswizzle, decode, scale to a
small pyramid (the bottom screen is 1240x1080: a few zoom levels of 256 px tiles are plenty). No PC tooling, no game
assets in the repo, and it works for any game version. If the tiles turn out to be bntx in a zstd-with-dictionary
archive, the same path needs SARC parsing (trivial) and the dictionaries from `Pack/ZsDic.pack.zs`
(`ZSTD_decompress_usingDict`).

Drawing: the bottom screen renders in software; blit the visible tiles from the cache, then the Link marker (a
triangle rotated by the heading). Redraw on a pan / zoom gesture or when Link moved more than a few pixels.

### Found in step B (TOTK_FS_LOG session, 2026-10-23)

The map is not TexToGo: it is `UI/Map/MainField/<layer>_<col>-<row>_<level>[_<reveal>].bntx.zs`, BNTX in zstd with the
`zs.zsdic` dictionary (from `Pack/ZsDic.pack.zs`, a plain-zstd SARC that also holds `bcett.byml.zsdic`, `pack.zsdic`).
- layer: `G` ground, `S` sky, `U` Depths (underground; one version per tile, no reveal suffix); `Grid_00..03` overlays
- col 00..11, row 00..09: tile (c, r) covers x -6000+1000c .. +1000, z -5000+1000r .. +1000 (checked: G_05-04 holds
  Lookout Landing, where Link stands at x -254, z -93)
- level: 00 3000x3000 ASTC 12x12 sRGB (block height 16), 01 (~750), 02 300x300 ASTC 8x8, 03 (~75), 04 27x27 ASTC 8x8
- reveal: one letter per region the tile overlaps, `T` revealed / `F` not (`TF`, `FT`, ...: 43 tiles in one region,
  50 in two, 21 in three, 2 in four); `_A` / `_B` alternatives on a few tiles (event changes; to look at). The all-`T`
  name is the fully revealed map; the real state needs the regions' GameData flags (step G).
- the sky's `T` tiles are mostly cloud with the islands drawn in; the Depths tiles are masks the game tints (magenta /
  grey channels): to work out before drawing them.

Item icons: `UI/Tex/Icon/<item id>.bntx.zs` (the save's ids directly: Weapon_Sword_070, Item_Fruit_P, NormalArrow),
256x256 ASTC 4x4 sRGB, block height 8. Names: `Mals/USen.Product.142.sarc.zs` (the version in the name).

BRTI fields used (from its start): +0x1C format (high byte: 0x2d..0x3a ASTC 4x4, 5x4, 5x5, 6x5, 6x6, 8x5, 8x6, 8x8,
10x5, 10x6, 10x8, 10x10, 12x10, 12x12; low byte 6 sRGB), +0x24 width, +0x28 height, +0x34 block height log2; one mip:
the data at BRTD + 0x10. Checked on the PC with a scratch decoder (scratchpad tools, not in the repo).

Still needed on the device: zstd (with dictionaries) and an ASTC decoder (nothing in the tree decodes ASTC); the
Tegra deswizzle is in third_party/d2r_gfx. Cache: level 03 for the whole map (~900 px across: the tab's full view),
02 / 01 decoded on demand when zoomed in, in the game folder's bottom-cache/.

## 3. Inventory data: GameData

All inventory lives in GameData, keyed by MurmurHash3-32 (seed 0) of the key name (verified:
`Pouch.Material.Content.Name` -> 0xd96ebf12, `Pouch.Weapon.Content.Name` -> 0x65efd0be). Key names and types, from
Marc Robledo's TOTK save editor (zelda-totk.class.pouch.js; 31k known keys in zelda-totk.hashes.csv):

- Weapon / Shield: `.Content.Name` String64Array, `.Life` IntArray, `.Effect.Type` EnumArray, `.Effect.Value` IntArray,
  `.Combined.Name` String64Array (the fused material), `.Combined.Life`, `.ExtraLife`
- Bow: `.Name`, `.Life`, `.Effect.Type`, `.Effect.Value`; Armor: `.Name`, `.ColorVariation` (dye)
- Arrow, Material, SpecialParts (Zonai devices), KeyItem: `.Content.Name` String64Array + `.Content.StockNum` IntArray
  (Material also `.GetOrder`/`.UseOrder`: the game's sort orders)
- Food: `.Name`, `.StockNum`, `.LifeRecover` (quarter hearts), `.Effect.Type`, `.Effect.Level`, `.Effect.Time` (s),
  `.Price`, `.MaterialName` (5 per dish)
- also useful: `OwnedHorseList.*`, `MapData.IconData.StampData.Type/Pos/Layer` (map stamps), rupees / hearts under
  `PlayerStatus.*` (look the exact names up in the hash list)

Item ids are actor names (`Weapon_Sword_001`, `Item_Fruit_A`, ...); empty slots are "".

**Two ways to read it, in order:**

1. **The save file (first version: no reverse engineering).** progress.sav is GameData in a documented format
   (TotkMods/Research `Formats/Sav`): magic 0x01020304, version, save-data offset; from 0x20 an array of
   `{u32 hash, u32 value-or-offset}` divided into typed sections (a section starts with hash 0 and the type enum:
   0x03 IntArray, 0x07 EnumArray, 0x11 String64Array, ...); array values point into the data section as `u32 count`
   then the elements (String64 = 64 bytes). The runtime writes saves itself (fs_host.c, under TOTK_PROFILE_DIR, one
   folder per slot), so it knows when a save finished: re-read the newest slot's progress.sav then. TOTK autosaves
   often, so the tab lags by minutes at worst - label it "as of the last save".
2. **Live, from memory (second version).** GameData lives in `gmd::GameDataMgr` (lotuskit resolves
   `gmd::GameDataMgr::sInstance` and calls the game's getters, e.g. `getIntArray`, `getStructString64Array`; its
   addresses are for 1.0.0 / 1.1.0 / 1.2.1 only). Calling game functions from the bottom screen's thread is risky
   (locks, guest thread context), so read the tables instead: find where the live values sit by scanning the game's
   heap for the known key hashes next to plausible data (e.g. 0xd96ebf12 followed by an offset / pointer to 64-byte
   strings starting "Item_" or "Obj_"), confirm against the save file's values, then read those arrays directly
   (host_safe_read) a few times a second. A diagnostic command (`memscan <hex u32>`: addresses and surrounding bytes
   in writable mappings) makes this a log-reading exercise for the agent. Once the layout is known it is likely the
   save format in memory (the save is written from it), version-independent apart from finding the table.

   **Done (step F, 2026-10-26, 1.4.3 only): runtime/gamedata_live.c.** Found with `memscan` / `memdump`
   (runtime/mem_scan.c, commands on input.txt) on the Thor. The layout differs from the save's: main+0x3ad66b0 holds
   the GameData object (vtable main+0x39403b0); at +8, one 0x60-byte container per value type, numbered as in the save
   (0 Bool with 0x53a6 keys, 3 IntArray, 17 String64Array); each has the values (+0x10), an open hash table of
   `{u32 hash, u32 index}` (+0x18; slot hash % size, the next slot on a clash, hash 0 empty) and its count and size
   (+0x20, +0x24). A value is an object (IntArray 0x40 bytes, String64Array 0x90) whose last four words are
   `{count, the live array, count, another copy}`; a String64 element is 0x50 bytes with the text at +0xc. Every step
   checks a vtable, so a different build reads nothing and the tab falls back to the save. The heap addresses were the
   same in three runs, but nothing relies on that. The Inventory tab reads it once a second ("live" in the footer); on
   the title screen the pouches are empty and the newest save is shown.

## 4. Item names and icons

- **Names:** the message archive `Mals/USen.Product.<ver>.sarc.zs` (zstd with the zs dictionary, a SARC) holds MSBT
  files; item names are labels `<ActorName>_Name` in the actor message files (e.g. `ActorMsg/PouchContent.msbt`; to
  confirm with TOTK_FS_LOG while the inventory is open). MSBT: LBL1 (labels -> index) + TXT2 (UTF-16 strings with
  0x0E control tags to strip). Pick the language file from the system locale, English fallback.
- **Icons:** found with TOTK_FS_LOG while browsing the inventory (see 2); decoded into the same cache as the map.
  Fused weapons: the base weapon's icon plus a small badge for the fuse is enough at first.

## 5. Plan

| step | result on the Thor | needs |
| --- | --- | --- |
| A | Map tab: a dot that moves with Link on a plain grid (coordinates, layer from height) | runtime_player_pose |
| B | TOTK_FS_LOG session: the list of map, icon and message files | The user opens map + inventory once |
| C | Map tab with the real map imagery, three layers, pan / zoom, Link marker with heading | zstd, txtg reader, tile cache |
| D | Inventory tab from the newest save: pouch tabs, counts, durability, names (ids first, then MSBT names) | sav reader |
| E | Inventory icons | the icon files from B |
| F | Inventory live from memory (no save lag) | done 2026-10-26 (gamedata_live.c, 1.4.3) |
| G | Map extras: stamps from GameData, the revealed / unexplored state | GameData keys |
| H | Equip weapons / shields from the Inventory tab | done 2026-10-26 (through the game's quick menu) |

A and D can start now; C and E wait on B (minutes of play with one setting on).

**Equipping (step H, 2026-10-26).** The Inventory tab's detail panel has an Equip button on Weapons and Shields while
the pouches are live. It equips through the game's own quick menu: hold the D-pad direction (right weapons, left
shields), flick the right stick once per slot from the equipped one (`Pouch.<key>.EquipIndex[0]`; from "none" the
menu starts at slot 0), let go; then it reads the equipped slot back and walks the difference again, up to three tries.
Verified on the Thor for weapons, forwards and backwards. The game equips nothing for an item at durability 0 (the
Decayed Master Sword recharging) - choosing it unequips - so its button is off. Not done: bows (ZR + D-pad right, and
letting go of ZR shoots), holding materials (the + menu). Calling the game's equip function directly was tried first:
`memwatch` (mem_scan.c) shows the equip index is written by the generic GameData setter from a per-frame flush of
queued writes (main+0x3e59a4 -> 0x3e5dfc -> 0x74240), so the caller that queued it is not on that stack; lotuskit has
no equip symbols either.

**Done (2026-10-23):** A: `runtime_player_pose` (world_survey.c, read-only, any thread) and the Map tab's grid with
Link's marker, layer and coordinates, redrawn 10 times a second (heading sign still to confirm in play). D, first
half: `runtime/bottom_inventory.c` reads the newest slot's progress.sav (re-read when its time changes) and lists the
nine pouches with counts / durability / fuse, item ids for now; pouch chips and swipe scrolling. Names (MSBT) wait on B.
C and E, first version (2026-10-23): runtime/bottom_assets.c reads the tiles and icons from the RomFS in-process
(vendored third_party/zstd 1.5.6 single-file decoder with zs.zsdic, third_party/astc from texture2ddecoder, our
block-linear walk), builds one 1200x1000 picture per layer from the level-02 tiles on a thread when the layer is first
shown (Thor: well under a second; the PC test 0.4 s for all three), and decodes each icon at 48 px on first draw. The
map shows the fully revealed tiles (`_A` where only `_A` / `_B` exist); the Depths in grey from their mask's luma.
Names (2026-10-23): item_name reads ActorMsg/PouchContent.msbt from Mals/USen.Product.<n>.sarc.zs (<n> found by
trying; 142 on 1.4.3), the `<id>_Name` labels (1365), UTF-16 to UTF-8; the bottom screen's text reads UTF-8
("Sautéed Peppers"). 533 of the 535 ids in the test save have one; Weapon_Sword_077 and Armor_1150_Upper have none in
any message file (variants that show another item's name in game) and show their ids. English only for now.
Zoom and pan (2026-10-23, runtime/bottom_map.c): pinch, drag, double-tap (x2 at the point), "Whole map" and "Follow
Link" (keeps Link centred; a drag turns it off). From the whole map (~82 px a tile) to 1500 px a tile; a tile shown
larger than the 100 px overview is decoded from level 02 / 01 / 00 (00 kept at 1500 px) on one thread into an LRU
cache of 80 MB, the overview drawn meanwhile; the overview's builder records each tile's file version for the levels.
Checked on the Thor with adb taps / swipes (double-tap, pan, follow); pinch needs fingers.
Still open: the language; the Depths' reveal (lightroots).
Look (2026-10-26): closer to the game's menus. Near-black panels with cut corners and an olive edge, cream text, gold
for values, the selection in Zonai green with corner brackets and a soft glow, gold rules and diamonds as ornaments,
and the text in the Switch's standard shared font (`extracted/firmware/shared-fonts/0-standard.bfttf`, decoded in
memory; Roboto when it is missing). Text sizes are by the em scaled to Roboto's metrics, so the layout fits either font.
Checked with a PC preview that draws the tabs from a real save and the game's icons and tiles (scratch tool, not in the repo).
Inventory grid (2026-10-26): the pouch as a grid of the game's icons, 7 across (the count on a shade, a fused material as
a small badge), a tap picks an item; beside it the picked item: its icon large, name, durability or count, fused
material, and the game's description (`<id>_Caption` in ActorMsg/PouchContent.msbt: 1360 of them).
Depths (2026-10-26): the tiles' channels are relief (blue), gloom (magenta: red and blue over green) and a few features in
their own colours; tinted as dark stone, maroon gloom, the features kept (`depths_tint`). The game's exact tint may differ.
Reveal, step G (2026-10-26): the surface and the sky follow the newest save's Skyview Towers. GameData
`IsActivateCannon.Tower01..15` (Bool) is the tower launched from; region n's outline is
`UI/Map/MainFieldOpenMask/G_OpenMask_NN` (also S_, with _A / _B), a tile-aligned box at 135 px a tile (level 03's size).
The boxes' places are not in any file found (the layout archives do not have them): they were found on the PC by
matching each mask with the pixels that differ between tiles' T and F versions (scores 0.87..0.98, 117 of 120 tiles'
letter counts reproduced), and are a table in bottom_assets.c (`g_mask_at`, region n -> tile column, row). A tile's
letters are the masks over it, ascending (in every tile the matching could tell). The sky's tiles have the same letter
counts (118 of 120), so the same regions. Region n = Tower n is assumed (region 1 is Lookout Landing's, where tower 1 is).
The map bar's "Reveal all" shows the whole map. Sky tiles that come as `_A_A`.. take that over `_A` (the islands alone,
which drew as a blue checkerboard).

## Sources

- aquacluck/totk-lotuskit: player actor lookup (src/program/util/player.cpp, world.cpp), GameDataMgr access
  (src/program/util/gamedata.cpp, syms/exking/gmd/GameDataMgr.py)
- marcrobledo/savegame-editors, zelda-totk: pouch keys (zelda-totk.class.pouch.js), key hash list
  (zelda-totk.hashes.csv), map stamps (zelda-totk.coordinates.js)
- TotkMods/Research: Formats/Sav (save format), Formats/TexToGo
- KillzXGaming/Switch-Toolbox: File_Format_Library/FileFormats/Texture/TXTG.cs (TexToGo reader)
- zeldamods/objmap-totk (web map; its tiles are hosted separately, not in the repo)
