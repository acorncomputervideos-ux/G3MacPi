#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/diskimage/ndif_extract.py — turns a MacBinary-encoded Disk Copy 6
# image (.img, or a self-mounting .smi) into a raw volume image that
# tools/hfs (g3_hfs) can read. Host-side helper; nothing here is Apple's.
#
#   ndif_extract.py IN.bin OUT.img
#
# Formats (see docs/tools/diskimage.md for sources and what is verified):
# - MacBinary II header (128 bytes): name length @1, name @2, type @65,
#   creator @69, data fork length @83, resource fork length @87; the data
#   fork follows the header, the resource fork follows it, each padded to
#   128 bytes.
# - Resource fork: Inside Macintosh: More Macintosh Toolbox, "Resource
#   Manager" (header, map, type list, reference lists).
# - The 'bcem' resource 128 (Disk Copy 6 block map), as found in Apple's
#   images: sector count u32 @68, chunk count u32 @124, then chunks of 12
#   bytes from @128: start sector (3 bytes), type (1 byte), data-fork
#   offset u32, length u32. Types: 0x00 zeros, 0x02 stored, 0x83 ADC,
#   0xFF end. Each chunk runs to the next chunk's start sector.
# - ADC (Apple Data Compression): byte b >= 0x80: (b & 0x7F) + 1 literal
#   bytes follow; 0x40 <= b < 0x80: copy (b & 0x3F) + 4 bytes from
#   distance (next 2 bytes, big-endian) + 1; b < 0x40: copy ((b >> 2) & 15)
#   + 3 bytes from distance ((b & 3) << 8 | next byte) + 1.
import struct
import sys


def adc(src, out_len):
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < out_len:
        b = src[i]
        if b & 0x80:
            n = (b & 0x7F) + 1
            out += src[i + 1:i + 1 + n]
            i += 1 + n
            continue
        if b & 0x40:
            n, dist = (b & 0x3F) + 4, ((src[i + 1] << 8) | src[i + 2]) + 1
            i += 3
        else:
            n, dist = ((b >> 2) & 0xF) + 3, (((b & 3) << 8) | src[i + 1]) + 1
            i += 2
        if dist > len(out):
            raise ValueError('ADC distance before the start of the output')
        for _ in range(n):
            out.append(out[-dist])
    return bytes(out)


def resources(fork):
    data_off, map_off, _, map_len = struct.unpack('>IIII', fork[:16])
    m = fork[map_off:map_off + map_len]
    tl = struct.unpack('>H', m[24:26])[0]
    ntypes = struct.unpack('>H', m[tl:tl + 2])[0] + 1
    res = {}
    for t in range(ntypes):
        rtype, count, ref = struct.unpack('>4sHH', m[tl + 2 + 8 * t:tl + 10 + 8 * t])
        for k in range(count + 1):
            rid, _, attr_off = struct.unpack('>hHI', m[tl + ref + 12 * k:tl + ref + 12 * k + 8])
            off = data_off + (attr_off & 0xFFFFFF)
            length = struct.unpack('>I', fork[off:off + 4])[0]
            res[(rtype, rid)] = fork[off + 4:off + 4 + length]
    return res


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: ndif_extract.py IN.bin OUT.img')
    b = open(sys.argv[1], 'rb').read()
    if len(b) < 128 or b[0] != 0 or not 1 <= b[1] <= 63:
        sys.exit('not a MacBinary file')
    dlen, rlen = struct.unpack('>II', b[83:91])
    data = b[128:128 + dlen]
    roff = 128 + (dlen + 127) // 128 * 128
    res = resources(b[roff:roff + rlen])
    m = res.get((b'bcem', 128))
    if m is None:
        sys.exit("no 'bcem' 128 resource: not a Disk Copy 6 image")
    total = struct.unpack('>I', m[68:72])[0]
    n = struct.unpack('>I', m[124:128])[0]
    chunks = []
    for k in range(n):
        e = m[128 + 12 * k:140 + 12 * k]
        off, length = struct.unpack('>II', e[4:12])
        chunks.append(((e[0] << 16) | (e[1] << 8) | e[2], e[3], off, length))
    img = bytearray(total * 512)
    for k, (sector, kind, off, length) in enumerate(chunks):
        if kind == 0xFF:
            break
        want = (chunks[k + 1][0] - sector) * 512
        if kind == 0x00:
            chunk = bytes(want)
        elif kind == 0x02:
            chunk = data[off:off + length]
        elif kind == 0x83:
            chunk = adc(data[off:off + length], want)
        else:
            sys.exit('chunk %d: unknown type 0x%02X' % (k, kind))
        if len(chunk) != want:
            sys.exit('chunk %d: %d bytes, expected %d' % (k, len(chunk), want))
        img[sector * 512:sector * 512 + want] = chunk
    open(sys.argv[2], 'wb').write(img)
    print('%d sectors, %d chunks' % (total, n))


main()
