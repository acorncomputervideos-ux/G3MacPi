#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/diskimage/extract_mac_rom.py — copies the "Mac OS ROM" file (or any
# named file) out of a Mac OS CD or disk image: a raw .iso / .toast / .cdr
# / .img holding an HFS or HFS Plus volume, partitioned or not. Standalone
# (Python 3 standard library only), so it can run on any computer.
#
#   python3 extract_mac_rom.py MacOS9.iso                 -> "Mac OS ROM"
#   python3 extract_mac_rom.py MacOS9.iso "Finder" Finder.bin
#
# It writes the file's data fork (what the firmware loads) and, if there is
# one, its resource fork next to it as "<out>.rsrc".
#
# Formats: HFS — Inside Macintosh: Files; HFS Plus — Apple TN1150. The
# volume is found by scanning for a Master Directory Block ('BD') or
# volume header ('H+' / 'HX') 1024 bytes after a 512-byte boundary, so
# partition maps with either 512- or 2048-byte blocks need no parsing.
# Files are found by walking the catalog B-tree's leaf nodes.
import struct
import sys

be16 = lambda b, o: struct.unpack_from('>H', b, o)[0]
be32 = lambda b, o: struct.unpack_from('>I', b, o)[0]
be64 = lambda b, o: struct.unpack_from('>Q', b, o)[0]


class Image:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, 2)
        self.size = self.f.tell()

    def read(self, pos, n):
        self.f.seek(pos)
        return self.f.read(n)


def find_volumes(img):
    """Yields (offset, kind) for every plausible HFS / HFS Plus volume."""
    step = 512
    pos = 0
    while pos + 1536 <= img.size:
        sig = img.read(pos + 1024, 2)
        if sig == b'BD':
            mdb = img.read(pos + 1024, 162)
            blk = be32(mdb, 20)
            if blk and blk % 512 == 0 and mdb[36] <= 27:
                embed = mdb[0x7C:0x7E]
                if embed in (b'H+', b'HX'):  # HFS Plus inside an HFS wrapper
                    start = be16(mdb, 0x7E)
                    yield pos + be16(mdb, 28) * 512 + start * blk, 'plus'
                else:
                    yield pos, 'hfs'
        elif sig in (b'H+', b'HX'):
            vh = img.read(pos + 1024, 512)
            if be16(vh, 2) in (4, 5) and be32(vh, 40) and be32(vh, 40) % 512 == 0:
                yield pos, 'plus'
        # Volumes start on partition boundaries; scanning every 512 bytes of a
        # large image is slow, so after the first megabyte scan 2 KB steps.
        pos += step if pos < (1 << 20) else 2048


class Volume:
    def __init__(self, img, base, kind):
        self.img, self.base, self.plus = img, base, kind == 'plus'
        if self.plus:
            vh = img.read(base + 1024, 512)
            self.blk = be32(vh, 40)
            self.alloc = base
            self.cat_fork = self.fork_plus(vh, 272)
            self.ext_fork = self.fork_plus(vh, 192)
        else:
            mdb = img.read(base + 1024, 162)
            self.blk = be32(mdb, 20)
            self.alloc = base + be16(mdb, 28) * 512
            self.name = mdb[37:37 + mdb[36]].decode('mac_roman')
            self.cat_fork = (be32(mdb, 146), self.extents_hfs(mdb, 150))
            self.ext_fork = (be32(mdb, 130), self.extents_hfs(mdb, 134))

    @staticmethod
    def extents_hfs(b, o):
        return [(be16(b, o + 4 * i), be16(b, o + 4 * i + 2)) for i in range(3)]

    @staticmethod
    def fork_plus(b, o):
        return (be64(b, o), [(be32(b, o + 16 + 8 * i), be32(b, o + 20 + 8 * i)) for i in range(8)])

    def read_fork(self, fork):
        size, extents = fork
        out = bytearray()
        for start, count in extents:
            if len(out) >= size or count == 0:
                break
            out += self.img.read(self.alloc + start * self.blk, count * self.blk)
        if len(out) < size:
            raise RuntimeError('file is fragmented beyond its first extents (not supported)')
        return bytes(out[:size])

    def files(self):
        """Yields (name, type, creator, data fork, rsrc fork) for every file."""
        cat = self.read_fork(self.cat_fork)
        node_size = be16(cat, 14 + 18)
        node = be32(cat, 14 + 10)  # first leaf
        seen = set()
        while node and node not in seen:
            seen.add(node)
            n = cat[node * node_size:(node + 1) * node_size]
            nrec = be16(n, 10)
            for i in range(nrec):
                off = be16(n, node_size - 2 * (i + 1))
                if self.plus:
                    klen = be16(n, off)
                    name_len = be16(n, off + 6)
                    name = n[off + 8:off + 8 + 2 * name_len].decode('utf-16-be', 'replace')
                    r = off + 2 + klen
                    if be16(n, r) != 2:
                        continue
                    ftype, creator = n[r + 48:r + 52], n[r + 52:r + 56]
                    data = self.fork_plus(n, r + 88)
                    rsrc = self.fork_plus(n, r + 168)
                else:
                    klen = n[off]
                    name = n[off + 7:off + 7 + n[off + 6]].decode('mac_roman', 'replace')
                    r = off + 1 + klen
                    r += r & 1
                    if n[r] != 2:
                        continue
                    ftype, creator = n[r + 4:r + 8], n[r + 8:r + 12]
                    data = (be32(n, r + 26), self.extents_hfs(n, r + 74))
                    rsrc = (be32(n, r + 36), self.extents_hfs(n, r + 86))
                yield name, ftype, creator, data, rsrc
            node = be32(n, 0)  # next leaf


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: extract_mac_rom.py IMAGE ["File name" [OUT]]')
    want = sys.argv[2] if len(sys.argv) > 2 else 'Mac OS ROM'
    out = sys.argv[3] if len(sys.argv) > 3 else want
    img = Image(sys.argv[1])
    found_volume = False
    for base, kind in find_volumes(img):
        try:
            vol = Volume(img, base, kind)
            files = list(vol.files())
        except Exception:  # not really a volume (e.g. a backup MDB): keep scanning
            continue
        found_volume = True
        print('%s volume at byte %d: %d files' % ('HFS Plus' if vol.plus else 'HFS', base, len(files)))
        for name, ftype, creator, data, rsrc in files:
            if name.lower() == want.lower():
                d = vol.read_fork(data)
                open(out, 'wb').write(d)
                print('wrote "%s": %d bytes (type %s, creator %s)' % (out, len(d), ftype.decode('mac_roman'),
                                                                  creator.decode('mac_roman')))
                if rsrc[0]:
                    r = vol.read_fork(rsrc)
                    open(out + '.rsrc', 'wb').write(r)
                    print('wrote "%s.rsrc": %d bytes' % (out, len(r)))
                return
    sys.exit('no file named "%s" found%s' % (want, '' if found_volume else ' (no HFS volume found in the image)'))


main()
