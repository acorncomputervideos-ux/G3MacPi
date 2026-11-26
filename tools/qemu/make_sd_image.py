#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/qemu/make_sd_image.py — an SD card image for running the Pi kernel
# in QEMU's raspi4b machine (docs/qemu-pi.md): an MBR with one FAT32
# partition (type 0x0C) holding a tree of 8.3-named files, padded to a
# power of two (QEMU requires that for SD cards). Sparse output.
#
#   make_sd_image.py out.img total_mb DEST=SOURCE ... DIR/ ...
#     DEST: an 8.3 path such as G3MACPI/CD/CD921.ISO; "DIR/" an empty folder.
#
# No mkfs.fat needed. Directories hold at most 128 entries.
#   DEST is an 8.3 path like G3MACPI/CD/CD921.ISO; "DIR/" makes an empty dir.
import os, struct, sys

out, total_mb = sys.argv[1], int(sys.argv[2])
specs = sys.argv[3:]
bps, spc, reserved, nfats = 512, 8, 32, 2
csize = bps * spc
total = total_mb * 2048
fat_secs = 1
while True:
    data_secs = total - reserved - nfats * fat_secs
    clusters = data_secs // spc
    if (clusters + 2) * 4 <= fat_secs * bps:
        break
    fat_secs += 1
assert clusters >= 65525, clusters
data0 = (reserved + nfats * fat_secs) * bps

# Tree: path components -> node
class Node:
    def __init__(self, name, is_dir, src=None):
        self.name, self.is_dir, self.src, self.children, self.first = name, is_dir, src, [], 0
root = Node('', True)
def find_dir(parts):
    n = root
    for p in parts:
        c = next((x for x in n.children if x.name == p), None)
        if c is None:
            c = Node(p, True); n.children.append(c)
        n = c
    return n
for s in specs:
    if s.endswith('/'):
        find_dir([p for p in s.upper().strip('/').split('/')])
        continue
    dest, src = s.split('=', 1)
    parts = dest.upper().split('/')
    find_dir(parts[:-1]).children.append(Node(parts[-1], False, src))

fat = {}
next_cluster = [2]
def alloc(nbytes):
    n = max(1, (nbytes + csize - 1) // csize)
    first = next_cluster[0]
    for c in range(first, first + n):
        fat[c] = c + 1 if c < first + n - 1 else 0x0FFFFFFF
    next_cluster[0] += n
    return first
def size_of(node):
    return os.path.getsize(node.src)
# Directories: one cluster each holds 128 entries (enough here).
def assign(node, is_root=False):
    node.first = alloc(csize)
    for c in node.children:
        if c.is_dir:
            assign(c)
        else:
            c.first = alloc(size_of(c))
assign(root, True)
assert next_cluster[0] < clusters + 2

PART = 2048  # sectors before the partition
image_size = 1 << ((PART * 512 + total * bps - 1).bit_length())
f = open(out, 'wb')
f.truncate(image_size)
def write_at(off, data):
    f.seek(PART * 512 + off); f.write(data)
def cluster_off(c):
    return data0 + (c - 2) * csize
def name83(name):
    if '.' in name:
        b, e = name.split('.')
    else:
        b, e = name, ''
    assert len(b) <= 8 and len(e) <= 3, name
    return b.ljust(8).encode() + e.ljust(3).encode()
def entry(raw, attr, first, size):
    return struct.pack('<11sBBBHHHHHHHI', raw, attr, 0, 0, 0, 0, 0, first >> 16, 0, 0x5821, first & 0xFFFF, size)
def write_dir(node, parent_first, is_root):
    ents = []
    if not is_root:
        ents.append(entry(b'.          ', 0x10, node.first, 0))
        ents.append(entry(b'..         ', 0x10, 0 if parent_first == 2 else parent_first, 0))
    for c in node.children:
        ents.append(entry(name83(c.name), 0x10 if c.is_dir else 0x20, c.first, 0 if c.is_dir else size_of(c)))
    assert len(ents) <= csize // 32
    write_at(cluster_off(node.first), b''.join(ents))
    for c in node.children:
        if c.is_dir:
            write_dir(c, node.first, False)
        else:
            with open(c.src, 'rb') as s:
                off = cluster_off(c.first)
                while True:
                    chunk = s.read(1 << 20)
                    if not chunk:
                        break
                    write_at(off, chunk)
                    off += len(chunk)
write_dir(root, 2, True)

# Boot sector, FSInfo, backup.
bs = bytearray(512)
bs[0:3] = b'\xEB\x58\x90'; bs[3:11] = b'G3QEMU  '
struct.pack_into('<HBHBHHBHHHII', bs, 11, bps, spc, reserved, nfats, 0, 0, 0xF8, 0, 63, 255, 0, total)
struct.pack_into('<IHHIHH12sBBBI11s8s', bs, 36, fat_secs, 0, 0, root.first, 1, 6, b'\0' * 12, 0x80, 0, 0x29,
                 0x12345678, b'G3QEMU     ', b'FAT32   ')
bs[510:512] = b'\x55\xAA'
fsi = bytearray(512)
struct.pack_into('<I', fsi, 0, 0x41615252)
struct.pack_into('<III', fsi, 484, 0x61417272, clusters - (next_cluster[0] - 2), next_cluster[0])
struct.pack_into('<I', fsi, 508, 0xAA550000)
for base in (0, 6):
    write_at(base * bps, bytes(bs))
    write_at((base + 1) * bps, bytes(fsi))
# FATs (only the used part is non-zero).
used = next_cluster[0]
fatbytes = bytearray(used * 4)
struct.pack_into('<II', fatbytes, 0, 0x0FFFFFF8, 0x0FFFFFFF)
for c, v in fat.items():
    struct.pack_into('<I', fatbytes, c * 4, v)
for k in range(nfats):
    write_at((reserved + k * fat_secs) * bps, bytes(fatbytes))
mbr = bytearray(512)
struct.pack_into('<B3sB3sII', mbr, 446, 0x00, b'\xfe\xff\xff', 0x0C, b'\xfe\xff\xff', PART, total)
mbr[510:512] = b'\x55\xAA'
f.seek(0); f.write(mbr)
f.close()
print(f"{out}: {total_mb} MB FAT32, {clusters} clusters, {used - 2} used")
