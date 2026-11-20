"""Prototype: pull the figure icons and the figure table out of a Skylanders game's archives.

Usage:
  python extract_icons.py <archives folder> <output folder> [--size 128]

Reads ToyData/*_ToyData.igz from permanent*.pak (the game's list of every toy it knows) and
the collection icons from toycollectionmaterials.pak. Writes one PNG per figure/variant icon
and figures.json:

  {"toys": [{"id": 16, "name": "Spyro", "element": 7, "series": "Series 1",
             "icon": "Collection_Spyro_Normal",
             "variants": [{"variant": 6145, "deco": 1, "year": 1, "series": "Series 2",
                           "icon": "Collection_Spyro_Reposed"}, ...]}, ...],
   "icons": {"Collection_Spyro_Normal": "Collection_Spyro_Normal.png", ...}}

Variant value in a figure dump (bytes 0x1C-0x1D):
  year << 12 | reposed << 11 | in_game_variant << 10 | lightcore << 9 | supercharger << 8 | deco
"""
import argparse
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iga import IgaArchive  # noqa: E402
from igz import Igz  # noqa: E402
import texture  # noqa: E402


def variant_value(deco, year, flags):
    b0, b1, b2 = flags & 0xFF, (flags >> 8) & 0xFF, (flags >> 16) & 0xFF
    # Flag bytes as seen in CVariantIdentifier +0x14 (byte 3 is always 1).
    # byte1 = in-game variant (0x400), byte2 = reposed (0x800); byte0 still unconfirmed.
    return (year << 12) | (0x800 if b2 else 0) | (0x400 if b1 else 0) | (0x100 if b0 else 0) | (deco & 0xFF)


def parse_toydata(igz):
    """Every C*ToyData class (characters, vehicles, traps, magic items, Swap Force halves, ...)
    shares the base layout read here."""
    toys = []
    for off, t in igz.objects():
        if not (t.startswith("C") and t.endswith("ToyData")):
            continue
        toy = {
            "type": t[1:-len("ToyData")],
            "kind": igz.u32(off + 0x08),
            "id": igz.u32(off + 0x0C),
            "element": igz.u32(off + 0x10),
            "name": igz.string_at(off + 0x18),
            "series": igz.string_at(off + 0x28),
            "icon": igz.handle_at(off + 0x38),
            "variants": [],
        }
        toys.append((off, toy))
    # Variant lists: the toy's +0x30 points to a CVariantIdentifierList; read the
    # identifiers that follow it, up to the next list or toy.
    starts = sorted(o for o, _ in igz.objects())
    kinds = dict(igz.objects())
    for off, toy in toys:
        lst = igz.u32(off + 0x30) & 0x07FFFFFF
        if lst not in kinds:
            continue
        i = starts.index(lst) + 1
        while i < len(starts) and kinds[starts[i]] == "CVariantIdentifier":
            v = starts[i]
            deco, year, flags = igz.u32(v + 0x0C), igz.u32(v + 0x10), igz.u32(v + 0x14)
            if v + 0x28 not in igz.hnd_fields:  # variant without its own icon
                i += 1
                continue
            toy["variants"].append({
                "variant": variant_value(deco, year, flags),
                "deco": deco, "year": year, "flags": flags,
                "series": igz.string_at(v + 0x18),
                "icon": igz.handle_at(v + 0x28),
            })
            i += 1
    return [t for _, t in toys]


# Toys the game handles in code instead of ToyData (found by scanning every archive's names:
# no ToyData for them anywhere). They are listed only when the running game ships the icon,
# so older games still hide them.
EXTRA_TOYS = [
    # id, name, icon texture path below ui!Collections!ToyInventoryIcons!
    (235, "Imaginators Chest", "MagicItems!Collections_Chest"),
    (310, "Gryphon Park Observatory", "MagicItems!2016ExpansionToys!Collections_Observatory"),
    (311, "Enchanted Elven Forest", "MagicItems!2016ExpansionToys!Collections_TreeHouse"),
]


def decode_icon(arc, tpath, size):
    w, h, _levels, pix = Igz(arc.read_name(tpath)).image()
    bpp = len(pix) / (w * h)
    fmt = "BC1" if bpp < 0.75 else "BC3"
    rgba = texture.decode(fmt, w, h, pix, tiled=True)
    rgba = b"".join(rgba[y * w * 4:(y + 1) * w * 4] for y in range(h - 1, -1, -1))  # stored bottom-up
    if size and (w, h) != (size, size):
        rgba = texture.resize(rgba, w, h, size, size)
        w = h = size
    return w, h, rgba


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archives")
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=128)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    toys, groups = [], []
    for pak in sorted(glob.glob(os.path.join(args.archives, "permanent*.pak"))):
        arc = IgaArchive(pak)
        for i, name in enumerate(arc.names):
            if re.match(r"(?i)^ToyData/.*_ToyData\.igz$", name):
                group = parse_toydata(Igz(arc.read(i)))
                groups.append(os.path.basename(name)[:-len("_ToyData.igz")])
                for t in group:
                    t["group"] = groups[-1]
                toys += group
    print("toys:", len(toys), "from", len(groups), "ToyData files")

    mats = IgaArchive(os.path.join(args.archives, "toycollectionmaterials.pak"))
    wanted = {t["icon"] for t in toys if t["icon"]} | {v["icon"] for t in toys for v in t["variants"] if v["icon"]}
    icons, missing, failed = {}, [], []
    for icon in sorted(wanted):
        mpath = "materialInstances/ToyCollection/%s.igz" % icon
        if mpath.lower() not in mats.index:
            missing.append(icon)
            continue
        mat = Igz(mats.read_name(mpath))
        tex = next((p for p in mat.deps if p.startswith("textures:/")), None)
        if not tex:
            missing.append(icon)
            continue
        tpath = "textures/" + tex[len("textures:/"):]
        try:
            w, h, rgba = decode_icon(mats, tpath, args.size)
            texture.write_png(os.path.join(args.out, icon + ".png"), w, h, rgba)
            icons[icon] = icon + ".png"
        except Exception as e:
            failed.append((icon, str(e)))

    known = {t["id"] for t in toys}
    for fid, name, rel in EXTRA_TOYS:
        if fid in known:
            continue
        suffix = "toyinventoryicons!%s`tga,101.igz" % rel.lower()
        tpath = next((n for n in mats.names if n.lower().endswith(suffix)), None)
        if not tpath:
            continue  # this game does not ship the icon: leave the toy out
        icon = "Extra_" + rel.split("!")[-1]
        w, h, rgba = decode_icon(mats, tpath, args.size)
        texture.write_png(os.path.join(args.out, icon + ".png"), w, h, rgba)
        icons[icon] = icon + ".png"
        toys.append({"type": "Extra", "kind": 0, "id": fid, "element": 0, "name": name,
                     "series": None, "icon": icon, "variants": [], "group": icon})
    print("icons written:", len(icons), "missing:", len(missing), "failed:", len(failed))
    for f in failed[:10]:
        print("  failed", f)
    with open(os.path.join(args.out, "figures.json"), "w", encoding="utf-8") as f:
        json.dump({"toys": toys, "icons": icons, "missing": missing}, f, indent=1)


if __name__ == "__main__":
    main()
