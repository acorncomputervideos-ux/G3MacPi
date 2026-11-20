// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/disk_format.h — writes new, empty Mac disk images: an Apple
// partition map with one HFS (Mac OS Standard) volume, or a bare HFS volume.
//
// Facts (docs/storage/disk-format.md; big-endian throughout):
// Apple partition map — Inside Macintosh: Devices, "SCSI Manager":
//  - Block 0, driver descriptor map: sbSig 'ER' @0, sbBlkSize @2,
//    sbBlkCount @4, sbDrvrCount @16.
//  - Blocks 1..n, one entry each: pmSig 'PM' @0, pmMapBlkCnt @4,
//    pmPyPartStart @8, pmPartBlkCnt @12, pmPartName @16 (32), pmParType
//    @48 (32), pmLgDataStart @80, pmDataCnt @84, pmPartStatus @88.
// HFS — Inside Macintosh: Files, "Data Organization on Volumes":
//  - 512-byte blocks 0-1 boot blocks, 2 the Master Directory Block, then the
//    volume bitmap (one bit per allocation block, from drVBMSt), then the
//    allocation blocks from drAlBlSt; the alternate MDB is the second-last
//    512-byte block, the last one is reserved.
//  - MDB: drSigWord 'BD' @0, drCrDate @2, drLsMod @6, drAtrb @10,
//    drNmFls @12, drVBMSt @14, drAllocPtr @16, drNmAlBlks @18,
//    drAlBlkSiz @20, drClpSiz @24, drAlBlSt @28, drNxtCNID @30,
//    drFreeBks @34, drVN @36 (Str27), drVolBkUp @64, drVSeqNum @68,
//    drWrCnt @70, drXTClpSiz @74, drCTClpSiz @78, drNmRtDirs @82,
//    drFilCnt @84, drDirCnt @88, drFndrInfo @92, drXTFlSize @130,
//    drXTExtRec @134, drCTFlSize @146, drCTExtRec @150.
//  - B-trees with 512-byte nodes: node descriptor {fLink, bLink, kind,
//    height, numRecords}, record offsets from the end of the node; the
//    header node holds the header record, a 128-byte user record and the
//    node-allocation map.
//  - Catalog keys {keyLength u8, reserved u8, parentID u32, name Str31},
//    padded to an even length in leaf nodes. A new volume's catalog holds
//    the root folder (key: parent 1 + volume name; folder record, dirID 2)
//    and its thread (key: parent 2, empty name; parent 1 + volume name).

#ifndef G3_CORE_STORAGE_DISK_FORMAT_H
#define G3_CORE_STORAGE_DISK_FORMAT_H

#include "core/common/types.h"

namespace g3 {

struct HfsFormatOptions {
    const char* volume_name = "Untitled";  // 1-27 MacRoman characters, no ':'
    u32 date = 0;                          // seconds since 1904-01-01 (creation and modification)
};

// Writes an empty HFS volume over `bytes` bytes at `vol` (a multiple of 512,
// at least 800 KB). False if the size or name is unusable.
bool format_hfs(u8* vol, u64 bytes, const HfsFormatOptions& opt);

// Writes a whole disk: driver descriptor map (no drivers), a partition map
// (63 blocks: the map itself and one Apple_HFS partition) and an empty HFS
// volume in the rest of the disk.
bool format_apm_hfs(u8* disk, u64 bytes, const HfsFormatOptions& opt);

// Where format_apm_hfs puts the HFS partition (in 512-byte blocks).
constexpr u32 kApmHfsStart = 64;

} // namespace g3

#endif // G3_CORE_STORAGE_DISK_FORMAT_H
