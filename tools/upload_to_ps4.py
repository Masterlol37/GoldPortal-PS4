"""Copy what the on-console menu needs to the PS4 over GoldHEN FTP (port 2121).

  python tools/upload_to_ps4.py --ip 192.168.1.50 [--figures <dump folder>] [--plugin <prx>] [--icons <pack>]

--icons    icon pack folder (make_icon_pack.py)      -> /data/goldportal/icons/
           Not needed normally: the plugin reads the icons out of the running game by itself.
--figures  your .dump folder (subfolders are kept)   -> /data/goldportal/figures/
           Files already on the PS4 are NOT overwritten (they hold the game's progress)
           unless --overwrite-figures is given.
--plugin   goldportal.prx                            -> /data/GoldHEN/plugins/goldportal.prx
           The old one is downloaded to <prx folder>/goldportal_prev.prx first.
"""
import argparse
import ftplib
import io
import os


def connect(ip):
    f = ftplib.FTP()
    f.connect(ip, 2121, timeout=30)
    f.login()
    f.voidcmd("TYPE I")
    return f


def mkdirs(ftp, path, known):
    parts = path.strip("/").split("/")
    cur = ""
    for p in parts:
        cur += "/" + p
        if cur in known:
            continue
        try:
            ftp.mkd(cur)
        except ftplib.error_perm:
            pass
        known.add(cur)


def listing(ftp, path):
    """{name: size} of the files in path ({} if it doesn't exist)."""
    out = {}
    try:
        lines = []
        ftp.retrlines("LIST " + path, lines.append)
    except ftplib.all_errors:
        return out
    for ln in lines:
        parts = ln.split(None, 8)
        if len(parts) == 9 and not ln.startswith("d"):
            out[parts[8]] = int(parts[4])
    return out


def upload_tree(ftp, local, remote, exts, overwrite):
    known, sent, kept = set(), 0, 0
    for root, _dirs, files in os.walk(local):
        files = [f for f in files if f.lower().endswith(exts)]
        if not files:
            continue
        rel = os.path.relpath(root, local).replace("\\", "/")
        rdir = remote if rel == "." else remote + "/" + rel
        mkdirs(ftp, rdir, known)
        have = {} if overwrite else listing(ftp, rdir)
        for fn in sorted(files):
            if fn in have:
                kept += 1
                continue
            with open(os.path.join(root, fn), "rb") as fh:
                ftp.storbinary("STOR " + rdir + "/" + fn, fh)
            sent += 1
            if sent % 100 == 0:
                print("  ", sent, "files ...", flush=True)
    return sent, kept


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--icons")
    ap.add_argument("--figures")
    ap.add_argument("--overwrite-figures", action="store_true")
    ap.add_argument("--plugin")
    a = ap.parse_args()
    ftp = connect(a.ip)
    if a.icons:
        sent, _ = upload_tree(ftp, a.icons, "/data/goldportal/icons", (".rgba", ".txt"), True)
        print("icons: %d files uploaded" % sent)
    if a.figures:
        sent, kept = upload_tree(ftp, a.figures, "/data/goldportal/figures", (".dump",), a.overwrite_figures)
        print("figures: %d uploaded, %d already on the PS4 (kept)" % (sent, kept))
    if a.plugin:
        prev = os.path.join(os.path.dirname(os.path.abspath(a.plugin)), "goldportal_prev.prx")
        buf = io.BytesIO()
        try:
            ftp.retrbinary("RETR /data/GoldHEN/plugins/goldportal.prx", buf.write)
            with open(prev, "wb") as fh:
                fh.write(buf.getvalue())
            print("old plugin saved to", prev)
        except ftplib.all_errors as e:
            print("no old plugin downloaded:", e)
        with open(a.plugin, "rb") as fh:
            ftp.storbinary("STOR /data/GoldHEN/plugins/goldportal.prx", fh)
        print("plugin uploaded: %d bytes" % os.path.getsize(a.plugin))
    ftp.quit()


if __name__ == "__main__":
    main()
