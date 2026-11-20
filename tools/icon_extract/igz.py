"""Minimal reader for Alchemy IGZ object files (version 9, as in the PS4 Skylanders games).

Header: 'IGZ\\x01' (stored as 01 5A 47 49), version, type hash, platform (14 = PS4),
fixup count, then 16-byte section entries at 0x14: (unused, offset, size, alignment).
Section 0 holds the fixups, the others are memory pools with the objects and raw data.

Fixups are tagged blocks (magic, count, size, data start). The ones used here:
  TSTR  string table              TMET  type names       MTSZ  type sizes
  TDEP  dependencies (name, path) EXNM  external handles (pairs of string indices)
  RVTB  object starts             ROFS  pointer fields   RSTT  string fields
  RHND  handle fields
RVTB/ROFS/RSTT/RHND are packed: 4-bit nibbles, low 3 bits are data, bit 3 means "more",
each value is the delta to the previous offset divided by 4.
Pointers are (pool index << 27) | offset.
"""
import struct


def unpack_offsets(body, count):
    nibs = []
    for b in body:
        nibs.append(b & 0xF)
        nibs.append(b >> 4)
    out, prev, i = [], 0, 0
    for _ in range(count):
        v, s = 0, 0
        while True:
            n = nibs[i]
            i += 1
            v |= (n & 7) << s
            s += 3
            if not n & 8:
                break
        prev += v * 4
        out.append(prev)
    return out


class Igz:
    def __init__(self, data):
        self.raw = data
        if data[:4] != b"\x01ZGI":
            raise ValueError("not an IGZ file")
        self.version, _hash, self.platform = struct.unpack_from("<III", data, 4)
        self.sections = []
        for k in range(16):
            _u, off, size, _al = struct.unpack_from("<IIII", data, 0x14 + 16 * k)
            if off == 0:
                break
            self.sections.append((off, size))
        fo, fs = self.sections[0]
        self.fixups = {}
        p = fo
        while p < fo + fs:
            magic, cnt, size, start = struct.unpack_from("<4sIII", data, p)
            self.fixups[magic.decode("latin1")] = (cnt, data[p + start:p + size])
            p += size
        self.pools = [data[o:o + s] for o, s in self.sections[1:]]
        self.strings = self._strlist("TSTR")
        self.types = self._strlist("TMET")
        cnt, body = self.fixups.get("MTSZ", (0, b""))
        self.type_sizes = list(struct.unpack_from("<%dI" % cnt, body))
        cnt, body = self.fixups.get("EXNM", (0, b""))
        self.handles = [struct.unpack_from("<II", body, 8 * i) for i in range(cnt)]
        self.deps = self._strlist("TDEP")
        self.obj_offsets = self._offsets("RVTB")
        self.ptr_fields = set(self._offsets("ROFS"))
        self.str_fields = set(self._offsets("RSTT"))
        self.hnd_fields = set(self._offsets("RHND"))

    def _strlist(self, key):
        """NUL-terminated strings. TSTR/TMET pad each to 2 bytes and may contain empty
        entries; TDEP (name, path pairs) is packed without padding."""
        cnt, body = self.fixups.get(key, (0, b""))
        aligned = key != "TDEP" and self.version >= 8  # version 7 (Swap Force) packs everything
        if key == "TDEP":
            cnt *= 2
        out, p = [], 0
        while len(out) < cnt and p < len(body):
            e = body.index(b"\0", p)
            out.append(body[p:e].decode("latin1"))
            p = e + 1
            if aligned and p & 1:
                p += 1
        return out

    def _offsets(self, key):
        cnt, body = self.fixups.get(key, (0, b""))
        return unpack_offsets(body, cnt) if cnt else []

    # Offsets in the fixup lists are relative to the first pool; objects in this format
    # (ToyData, materials) live there. Textures keep igImage2 in a later pool; see image().
    def objects(self, type_name=None):
        pool = self.pools[0]
        for off in self.obj_offsets:
            if off + 8 > len(pool):
                continue
            t = struct.unpack_from("<Q", pool, off)[0]
            if t < len(self.types) and (type_name is None or self.types[t] == type_name):
                yield off, self.types[t]

    def u32(self, off, pool=0):
        return struct.unpack_from("<I", self.pools[pool], off)[0]

    def string_at(self, off):
        if off not in self.str_fields:
            return None
        return self.strings[struct.unpack_from("<Q", self.pools[0], off)[0]]

    def handle_at(self, off):
        """Name of the external object a handle field refers to (e.g. a material name)."""
        if off not in self.hnd_fields:
            return None
        idx = struct.unpack_from("<Q", self.pools[0], off)[0] & 0x7FFFFFFF
        if idx >= len(self.handles):
            return None
        _ns, name = self.handles[idx]
        name &= 0x7FFFFFFF
        return self.strings[name] if name < len(self.strings) else None

    def root_names(self):
        """{object offset: name} for the root objects (igObjectList at 0, igNameList in parallel).
        Lists are (count at +0x0C, data pointer at +0x20); names are (string, hash) pairs."""
        pool = self.pools[0]
        lists = [o for o, t in self.objects() if t in ("igObjectList", "igNameList")]
        objs = names = None
        for o in lists:
            t = self.types[struct.unpack_from("<Q", pool, o)[0]]
            cnt = self.u32(o + 0x0C)
            data = self.u32(o + 0x20) & 0x07FFFFFF
            if t == "igObjectList" and objs is None:
                objs = [self.u32(data + 8 * i) & 0x07FFFFFF for i in range(cnt)]
            elif t == "igNameList":
                names = [self.strings[struct.unpack_from("<Q", pool, data + 16 * i)[0]] for i in range(cnt)]
        if not objs or not names:
            return {}
        return dict(zip(objs, names))

    def image(self):
        """For a texture file: (width, height, pixel bytes) of its igImage2."""
        if "igImage2" not in self.types:
            raise ValueError("no igImage2 in file")
        t = self.types.index("igImage2")
        size = self.type_sizes[t]
        for pi, pool in enumerate(self.pools):
            # Version 9: type id at pool start, width/height at 0x48.
            # Version 7: type id at 0x10, width/height at 0x50.
            if pi == 0 or len(pool) < 0x58:
                continue
            if struct.unpack_from("<Q", pool, 0)[0] == t:
                dims = 0x48
            elif struct.unpack_from("<Q", pool, 0x10)[0] == t:
                dims = 0x50
            else:
                continue
            if True:
                w, h, depth, levels = struct.unpack_from("<HHHH", pool, dims)
                # The pixel data is the biggest remaining pool.
                pix = max((p for i, p in enumerate(self.pools) if i != pi), key=len)
                return w, h, levels, pix
        raise ValueError("igImage2 object not found")
