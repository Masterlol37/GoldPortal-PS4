# Figure icon extraction: file formats and tools

How the figure pictures of the on-screen menu are found in the games' own archives. No figure art
is ever shipped with Goldportal: every user's plugin reads the icons out of their own copy
(`ps4_plugin/source/iconx.c`, the C version of what these Python tools do).

The Python scripts here are research tools and generate the plugin's tables:
`make_icon_map.py` (-> `icon_map.h`, plus `extra_icon_map.txt`) and `make_tt_map.py` (-> `tt_icons.h`,
made from Trap Team version 01.01; another version may need it regenerated).

Only the standard library is used.

```bash
# archives: a copy of /app0/archives (only permanent*.pak and toycollectionmaterials.pak are read)
python tools/icon_extract/extract_icons.py <archives folder> <output folder> [--size 128]

# check which of your dumps the game knows, and which icon each one gets
python tools/icon_extract/match.py <output folder>/figures.json <dump folder> --list-unknown
```

On the PS4 the archives are at `/mnt/sandbox/pfsmnt/<TITLE>-app0-patch0-union/archives/`
(GoldHEN FTP, port 2121, while the game is running); inside the game process they are `/app0/archives/`.

## What we learned (Imaginators, CUSA05158)

| Step | Where | Format |
|---|---|---|
| Figure table | `permanent.pak` → `ToyData/<Name>_ToyData.igz` (320 files, 325 toys) | IGZ objects `C*ToyData` |
| Icon material | `toycollectionmaterials.pak` → `materialInstances/ToyCollection/<icon>.igz` | IGZ, depends on one texture |
| Icon texture | same archive → `textures/GuiStandard_diffuse,textures@!ui!Collections!ToyInventoryIcons!...igz` | `igImage2`, 256×256 BC3, PS4 tiled, bottom-up |

- **IGA archives** ([iga.py](iga.py)): version 11, 32 KiB blocks, raw deflate. All 8069 files in the
  three archives tested decompress.
- **IGZ files** ([igz.py](igz.py)): version 9, platform 14. Fixup lists are nibble-packed deltas ×4.
  `TSTR` strings are 2-byte aligned and may contain empty entries; `TDEP` is not aligned.
- **`C*ToyData` base layout** (characters, vehicles, traps, magic items, Swap Force halves, ...):
  `+0x0C` figure ID, `+0x10` element, `+0x18` name, `+0x28` series,
  `+0x30` → `CVariantIdentifierList`, `+0x38` handle of the collection icon material.
- **`CVariantIdentifier`**: `+0x0C` deco ID, `+0x10` year code, `+0x14` flag bytes, `+0x28` icon.
  Variant value in a dump (bytes 0x1C-0x1D) = `year<<12 | reposed<<11 | in_game<<10 | deco`
  (checked against real Spyro dumps: 0x1801, 0x2805, 0x2C02, 0x3810).
- **Textures** ([texture.py](texture.py)): BC blocks stored as 8×8-block tiles, Morton order inside a tile.

## Menu rule

A dump is listed only if the running game's ToyData knows its figure ID. Its icon is the variant
of the same ToyData group with exactly the dump's variant value, else the toy's own icon. Swap Force
bottoms use their top half's icon. This hides figures from later games and the Nintendo figures
(which the PS4 versions don't support) without any hand-made list.

Some toys are handled in game code and have no ToyData in any archive (all 8191 name tables
scanned). `EXTRA_TOYS` in [extract_icons.py](extract_icons.py) lists them with their icon; an entry
is used only when the game ships that icon. Imaginators: chests (235), level packs (310, 311).

Result for Imaginators against 1069 dumps (all six games): 1050 get an icon. Hidden: the
Nintendo figures, Debug Minion, templates, SuperChargers trophies and the Swap Force adventure
packs Sheep Wreck Island / Tower of Time.

## Swap Force (CUSA00024)

- `toycollectionimages.pak`, IGA v10 (big-endian). Materials `materialinstances/toycollection/<icon>.igz`
  (IGZ v7) depend on `textures/juicestandard_collections_<x>.igz`.
- Icons are 192×192 32-bit BGRA with **GCN 2D macro tiling** (pitch 256). The pixel pool has a
  256-byte prefix: the image is the last 256×192×4 bytes. Each 256-byte chunk is an 8×8 pixel tile
  (Morton order inside); chunk index bit *j* = parity(mask[j] & (tx | ty << 5)) with masks
  `023 042 084 108 190 040 020 010 100 200` (bits 0-2 are the P8_32x32_16x16 pipe: x3^x4^y3, x4^y4, x5^y5).
  Found with a GF(2) search over tile-edge smoothness (the padding columns must land on empty chunks).
- ToyData is XML (`toydata.pak`, `.igx`) with symbolic ids, so icons are matched by **name** instead:
  the plugin carries the Imaginators id/variant → icon-name table (`ps4_plugin/source/icon_map.h`,
  made by `make_icon_map.py`) and looks each name up in the running game. 122 icons; variants whose
  Swap Force icon has another name fall back to the figure's default icon.

## Trap Team (CUSA00697)

- No `archives` folder. The collection pictures are in `misc/ui_collection_champions.arc` and
  `misc/ui_collection_traps.arc`: IGA v11 whose files are named only `0x<hash>`, stored uncompressed.
  Each is an IGZ v8 (type names 2-byte aligned, like v9) with a 192×192 BC3 `igImage2` inside the first
  pool (width/height at object +0x40); the BC3 blocks are the **last** 36864 bytes of the pixel pool
  (256-byte prefix), same 8×8-block Morton tiling as Imaginators.
- The names are in the matching `.bld` (IGA v11, compression type 2 = **LZMA**: per block a u16
  length, 5 property bytes, then that many bytes of raw LZMA1). Its sprite level lists every picture as
  `UI_SKY4/Collection_Images/.../<id>_<variant>_<Name>.png` (traps `<id>_<variant>.png`) in string-table
  order, and the texture hashes appear in its script data in the same order (checked visually, and each
  record mentions its figure id).
- `make_tt_map.py` turns that into `ps4_plugin/source/tt_icons.h` (id, variant, hash - numbers only),
  so the plugin needs no LZMA: 347 pictures (288 champions, 59 traps). Swap Force bottoms use the
  top's picture. 474 of 506 Trap Team-era dumps get a picture (421 with their exact variant).

## On the console

`ps4_plugin/source/iconx.c` does all of the above in C (own raw-deflate decoder). At game start it
reads `/data/goldportal/icons/index.txt` (`source <title> <year>` line): with no pack, or one from an
older game, it extracts from `/app0/archives` in the background and replaces the index. A pack from
a newer game is kept. PC test: `bash ps4_plugin/test/iconx_test.sh <archives> <out> <title> <year>`.

## Open points

- The SuperChargers trophies (3500-3503) and Sheep Wreck Island / Tower of Time (3300, 3301) get their
  icons through `extra_icon_map.txt` (`Collection_Racepack_*`, `Collection_Expansion_*`).
- Creation crystals all get the "Sculpt_1" icon; the per-shape icons (`CreationVessel/...`) need a
  separate rule.
- Swap Force variants whose icon has a different name than in Imaginators (e.g. `_Reposed2013`) fall
  back to the figure's default icon: Swap Force's ToyData (`toydata.pak`, XML) uses symbolic ids and
  deco names, which would need their numeric values.
