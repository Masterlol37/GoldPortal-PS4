"""Match figure dumps to the icons a game knows (the rule the PS4 menu will use).

  python match.py <figures.json> <dump folder> [--list-unknown]

A dump is shown in the game's menu only if the game's ToyData knows its figure ID.
Icon choice: a variant of the same ToyData group with exactly this variant value,
else the toy's own icon.
"""
import argparse
import collections
import json
import os


class Catalog:
    def __init__(self, figures_json):
        with open(figures_json, encoding="utf-8") as f:
            j = json.load(f)
        self.icons = j["icons"]
        self.by_id = {t["id"]: t for t in j["toys"]}
        self.groups = collections.defaultdict(list)
        for t in j["toys"]:
            self.groups[t["group"]].append(t)

    def lookup(self, fig_id, variant):
        """-> (toy, icon name, how) or (None, None, 'unknown')."""
        toy = self.by_id.get(fig_id)
        if toy is None:
            return None, None, "unknown"
        if variant:
            for t in self.groups[toy["group"]]:
                for v in t["variants"]:
                    if v["variant"] == variant and v["icon"] in self.icons:
                        return toy, v["icon"], "variant"
        if toy["icon"] in self.icons:
            return toy, toy["icon"], "base"
        # Swap Force bottoms have no collection icon of their own: use their top half's.
        if toy["group"].startswith("Legs_"):
            for t in self.groups.get("Body_" + toy["group"][5:], []):
                if t["icon"] in self.icons:
                    return toy, t["icon"], "top half"
        return toy, None, "no icon"


def read_ids(path):
    with open(path, "rb") as f:
        b = f.read(0x20)
    if len(b) < 0x20:
        return None
    return b[0x10] | b[0x11] << 8, b[0x1C] | b[0x1D] << 8


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("figures")
    ap.add_argument("dumps")
    ap.add_argument("--list-unknown", action="store_true")
    ap.add_argument("--json", help="write per-dump results here")
    args = ap.parse_args()
    cat = Catalog(args.figures)
    stats = collections.defaultdict(collections.Counter)
    unknown = collections.defaultdict(list)
    rows = []
    for root, _dirs, files in os.walk(args.dumps):
        for fn in files:
            if not fn.lower().endswith((".dump", ".bin", ".sky")):
                continue
            p = os.path.join(root, fn)
            ids = read_ids(p)
            if not ids:
                continue
            top = os.path.relpath(p, args.dumps).split(os.sep)[0]
            toy, icon, how = cat.lookup(*ids)
            stats[top][how] += 1
            if how in ("unknown", "no icon"):
                unknown[top].append("%s (id %d, var 0x%04X)" % (fn, ids[0], ids[1]))
            rows.append({"file": os.path.relpath(p, args.dumps), "id": ids[0], "variant": ids[1],
                         "icon": icon, "how": how, "name": toy and toy["name"]})
    total = collections.Counter()
    for top in sorted(stats):
        s = stats[top]
        total.update(s)
        print("%-40s variant %4d  base %4d  top half %3d  unknown %4d  no icon %3d" % (
            top, s["variant"], s["base"], s["top half"], s["unknown"], s["no icon"]))
        if args.list_unknown:
            for u in unknown[top][:15]:
                print("      ", u)
    print("%-40s variant %4d  base %4d  top half %3d  unknown %4d  no icon %3d" % (
        "TOTAL", total["variant"], total["base"], total["top half"], total["unknown"], total["no icon"]))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
