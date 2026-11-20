"""Build the icon pack the on-console menu reads, from Imaginators' archives.

  python make_icon_pack.py <Imaginators archives folder> <pack folder> [--size 96]

Writes to <pack folder>:
  <icon>.rgba   size*size pixels, 8-bit RGBA, top row first, straight alpha
  index.txt     first line "size <n>", then one line per lookup: "<figure id> <variant> <icon>"
                variant -1 = the figure's default icon. The plugin looks up "id variant" first,
                then "id -1".
Copy the folder to /data/goldportal/icons/ on the PS4 (upload_to_ps4.py does that).
The Imaginators icons are used for every game; which figures a game lists is decided by
the plugin (variant year code vs. the running game).
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iga import IgaArchive  # noqa: E402
from igz import Igz  # noqa: E402
from extract_icons import parse_toydata, decode_icon, EXTRA_TOYS  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archives")
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=96)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    toys = []
    for pak in sorted(glob.glob(os.path.join(a.archives, "permanent*.pak"))):
        arc = IgaArchive(pak)
        for i, name in enumerate(arc.names):
            if re.match(r"(?i)^ToyData/.*_ToyData\.igz$", name):
                group = os.path.basename(name)[:-len("_ToyData.igz")]
                for t in parse_toydata(Igz(arc.read(i))):
                    t["group"] = group
                    toys.append(t)
    mats = IgaArchive(os.path.join(a.archives, "toycollectionmaterials.pak"))

    written = {}

    def icon_file(icon, tpath=None):
        """Decode one icon once; returns its pack name or None."""
        if icon in written:
            return written[icon]
        if tpath is None:
            mpath = "materialInstances/ToyCollection/%s.igz" % icon
            if mpath.lower() not in mats.index:
                written[icon] = None
                return None
            tex = next((p for p in Igz(mats.read_name(mpath)).deps if p.startswith("textures:/")), None)
            if not tex:
                written[icon] = None
                return None
            tpath = "textures/" + tex[len("textures:/"):]
        w, h, rgba = decode_icon(mats, tpath, a.size)
        with open(os.path.join(a.out, icon + ".rgba"), "wb") as f:
            f.write(rgba)
        written[icon] = icon
        return icon

    groups = {}
    for t in toys:
        groups.setdefault(t["group"], []).append(t)
    lines = []
    for t in toys:
        base = t["icon"] and icon_file(t["icon"])
        if not base and t["group"].startswith("Legs_"):   # Swap Force bottoms: top half's icon
            for b in groups.get("Body_" + t["group"][5:], []):
                base = b["icon"] and icon_file(b["icon"])
                if base:
                    break
        if base:
            lines.append("%d -1 %s" % (t["id"], base))
        for g in groups[t["group"]]:              # variants anywhere in the same ToyData file
            for v in g["variants"]:
                vi = v["icon"] and icon_file(v["icon"])
                if vi:
                    lines.append("%d %d %s" % (t["id"], v["variant"], vi))
    known = {t["id"] for t in toys}
    for fid, _name, rel in EXTRA_TOYS:
        if fid in known:
            continue
        suffix = "toyinventoryicons!%s`tga,101.igz" % rel.lower()
        tpath = next((n for n in mats.names if n.lower().endswith(suffix)), None)
        if tpath:
            lines.append("%d -1 %s" % (fid, icon_file("Extra_" + rel.split("!")[-1], tpath)))
    lines = sorted(set(lines), key=lambda l: (int(l.split()[0]), int(l.split()[1])))
    with open(os.path.join(a.out, "index.txt"), "w", encoding="ascii", newline="\n") as f:
        f.write("size %d\n" % a.size)
        f.write("\n".join(lines) + "\n")
    n = sum(1 for v in written.values() if v)
    print("icons:", n, "index lines:", len(lines), "size: %.1f MB" % (n * a.size * a.size * 4 / 1e6))


if __name__ == "__main__":
    main()
