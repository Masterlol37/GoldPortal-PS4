"""Reader for Alchemy engine IGA archives (.pak), as used by the Skylanders games.

Layout of version 11 (SuperChargers / Imaginators, PS4), worked out from the files:

  header (0x38 bytes)
    0x00 magic 'IGA\\x1a'     0x04 version
    0x08 toc size            0x0C file count
    0x10 sector size         0x14 hash search divider
    0x18 hash search slop    0x1C/0x20/0x24 block table sizes (u32 / u16 / u8 entries)
    0x28 name table offset (u64)   0x30 name table size   0x34 flags
  toc
    u32  hash[count]
    file[count]: u64 (offset:40, ordinal:24), u32 length, u32 block index
    u32 large[], u16 medium[], u8 small[]   block tables
  name table: u32 offset[count], then for each file "build path\\0logical path\\0"

Files are split into 32 KiB blocks. The block index's top nibble is the compression
(0xF... = stored, 1 = raw deflate). Its low 28 bits index the block table that fits the
file's on-disk span. Each block entry is (sector offset from the file start) with the
top bit set when the block is compressed; a compressed block starts with a u16 length.
"""
import lzma
import struct
import zlib

BLOCK = 0x8000


class IgaArchive:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        # Version 11 (SuperChargers/Imaginators) is little-endian. Version 10 (Swap Force on
        # PS4) is big-endian ('\x1aAGI'), keeps the ordinal in its own u32 before the offset,
        # and stores one name per file followed by a u32.
        if d[:4] == b"IGA\x1a":
            e = "<"
        elif d[:4] == b"\x1aAGI":
            e = ">"
        else:
            raise ValueError("not an IGA archive")
        self.endian = e
        (_magic, self.version, toc, n, self.sector, _div, _slop,
         nl, nm, ns, nto, nts, _flags) = struct.unpack_from(e + "4sIIIIIIIIIQII", d, 0)
        if self.version not in (10, 11):
            raise ValueError("IGA version %d not supported yet" % self.version)
        o = 0x38 + 4 * n
        self.files = []
        for i in range(n):
            if self.version == 11:
                q, ln, bi = struct.unpack_from(e + "QII", d, o + 16 * i)
                self.files.append((q & 0xFFFFFFFFFF, ln, bi))
            else:
                _ordinal, off, ln, bi = struct.unpack_from(e + "IIII", d, o + 16 * i)
                self.files.append((off, ln, bi))
        o += 16 * n
        self.large = struct.unpack_from(e + "%dI" % nl, d, o); o += 4 * nl
        self.medium = struct.unpack_from(e + "%dH" % nm, d, o); o += 2 * nm
        self.small = d[o:o + ns]
        nt = d[nto:nto + nts]
        offs = struct.unpack_from(e + "%dI" % n, nt, 0)
        self.names = []
        for s in offs:
            first_end = nt.index(b"\0", s)
            if self.version == 11:  # "build path\0logical path\0"
                logical_end = nt.index(b"\0", first_end + 1)
                name = nt[first_end + 1:logical_end]
            else:
                name = nt[s:first_end]
            self.names.append(name.decode("latin1").replace("\\", "/"))
        self.index = {name.lower(): i for i, name in enumerate(self.names)}

    def _table_for(self, bi, nblocks):
        idx = bi & 0x0FFFFFFF
        # Which table a file uses depends on its on-disk span; the index is unique per table,
        # so pick the one sized for it (small: <=127 sectors, medium: <=32767, else large).
        for table, flag in ((self.small, 0x80), (self.medium, 0x8000), (self.large, 0x80000000)):
            if idx + nblocks <= len(table):
                yield table, flag

    def read(self, i):
        off, ln, bi = self.files[i]
        if bi == 0xFFFFFFFF:
            return self.data[off:off + ln]
        comp = bi >> 28
        nblocks = (ln + BLOCK - 1) // BLOCK
        idx = bi & 0x0FFFFFFF
        last_err = None
        for table, flag in self._table_for(bi, nblocks):
            try:
                return self._read_blocks(off, ln, comp, table[idx:idx + nblocks], flag)
            except Exception as e:  # wrong table guess: try the next size
                last_err = e
        raise ValueError("cannot read %s: %s" % (self.names[i], last_err))

    def _read_blocks(self, off, ln, comp, entries, flag):
        out = bytearray()
        for b, v in enumerate(entries):
            want = min(BLOCK, ln - len(out))
            p = off + (v & (flag - 1)) * self.sector
            if v & flag:
                clen = struct.unpack_from("<H", self.data, p)[0]
                if comp == 1:
                    chunk = zlib.decompress(self.data[p + 2:p + 2 + clen], -15)
                elif comp == 2:
                    # LZMA (Trap Team): 5 property bytes, then clen bytes of raw LZMA1 data.
                    props = self.data[p + 2:p + 7]
                    lc, rest = props[0] % 9, props[0] // 9
                    f = [{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": rest % 5, "pb": rest // 5,
                          "dict_size": struct.unpack_from("<I", props, 1)[0]}]
                    dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=f)
                    chunk = dec.decompress(self.data[p + 7:p + 7 + clen], max_length=want)
                else:
                    raise ValueError("compression type %d not supported" % comp)
            else:
                chunk = self.data[p:p + want]
            if len(chunk) != want:
                raise ValueError("block %d: got %d bytes, want %d" % (b, len(chunk), want))
            out += chunk
        return bytes(out)

    def read_name(self, name):
        return self.read(self.index[name.lower()])
