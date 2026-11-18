// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/mmu.h — Open Firmware's Virtual-Mode memory
// management: the translations it has established, the hashed page table
// that makes them real, and the "mmu" (CPU node) and "memory" package
// methods that scripts and clients use.
//
// Facts — PowerPC Processor Binding to IEEE 1275, rev. 2.1 (§4.2, §5.1.7,
// §6; docs/firmware/virtual-mode.md):
//  - Virtual-Mode (real-mode? false): firmware and client share one
//    virtual address space; addresses passed between them are virtual.
//  - The package that /chosen's "mmu" refers to (a CPU node) carries
//    "translations": sets of (virt size phys mode), each encode-int.
//  - mode is WIMGxPP as in a PTE (W 0x40, I 0x20, M 0x10, G 0x08, x 0,
//    PP 0x03). mode -1 (map-in, modify): system memory W=0 I=0 M=0 G=1
//    PP=10; I/O W=1 I=1 M=0 G=1 PP=10.
// MMU methods (IEEE 1275 §3.6.5, one phys cell on 32-bit PowerPC):
//    claim ( [virt] size align -- base )   release ( virt size -- )
//    map ( phys virt size mode -- )        unmap ( virt size -- )
//    modify ( virt size mode -- )          translate ( virt -- false | phys mode true )
// Memory node methods: claim ( [phys] size align -- base ), release ( phys size -- ).
//
// Hashed page table (PEM ch. 7): PTE word 0 = V | VSID << 7 | H << 6 |
// API (EA bits 4-9); word 1 = RPN | R | C | WIMG | PP; PTEG addresses as
// in core/cpu/ppc750/ppc750_mmu.cpp; SDR1 = HTABORG | HTABMASK.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_MMU_H
#define G3_CORE_FIRMWARE_OPENFIRM_MMU_H

#include "core/common/types.h"
#include "core/cpu/ppc750/memory_interface.h"
#include "core/firmware/openfirm/device_tree.h"
#include "core/firmware/openfirm/instances.h"
#include "core/firmware/openfirm/memory_map.h"

namespace g3::of {

namespace wimg {
constexpr u32 W = 0x40, I = 0x20, M = 0x10, G = 0x08, PP_RW = 0x02;
constexpr u32 kMemoryDefault = G | PP_RW;          // binding §6, mode -1, system memory
constexpr u32 kIoDefault = W | I | G | PP_RW;      // binding §6, mode -1, I/O
} // namespace wimg

struct Translation {
    u32 virt, size, phys, mode;
};

// The translations Open Firmware has established: page-granular,
// non-overlapping, ascending by virtual address.
class Translations {
public:
    static constexpr u32 kMax = 128;
    static constexpr u32 kPage = 4096;

    // Maps [virt, virt + size) to phys (virt and phys rounded down to a
    // page, the end rounded up). Replaces what was mapped there. False if
    // the table is full, the range wraps, or virt/phys are misaligned
    // relative to each other.
    bool map(u32 phys, u32 virt, u32 size, u32 mode);
    void unmap(u32 virt, u32 size);
    // Changes the mode of mapped pages in the range; false if none is mapped.
    bool modify(u32 virt, u32 size, u32 mode);
    bool translate(u32 virt, u32& phys, u32& mode) const;

    u32 count() const { return n_; }
    const Translation& operator[](u32 i) const { return t_[i]; }

private:
    bool insert(const Translation& t);
    Translation t_[kMax] = {};
    u32 n_ = 0;
};

// Builds a hashed page table in guest memory.
class PageTable {
public:
    // base: physical, aligned to size; size: 64 KB << n (n <= 9).
    PageTable(ppc::MemoryInterface& phys, u32 base, u32 size) : mem_(phys), base_(base), size_(size) {}
    u32 sdr1() const { return base_ | ((size_ >> 16) - 1); }
    bool clear();
    // Adds a PTE for the page at ea (segment vsid) -> pa, word-1 bits from mode;
    // primary group first, then secondary. False if both groups are full.
    bool insert(u32 vsid, u32 ea, u32 pa, u32 mode);
    u32 entries() const { return entries_; }

private:
    u32 pteg(u32 hash) const;
    ppc::MemoryInterface& mem_;
    u32 base_, size_;
    u32 entries_ = 0;
};

// Methods of the CPU node that /chosen "mmu" names. Virtual space is handed
// out from `virt_free`; every change updates "translations" on `node` and
// calls the sync callback (which makes the hardware follow).
class MmuDriver final : public PackageDriver {
public:
    using Sync = bool (*)(void* context);  // false: the translation couldn't be installed
    MmuDriver(DeviceTree& tree, Translations& t, MemoryMap& virt_free) : tree_(tree), t_(t), virt_(virt_free) {}
    void attach(Phandle node, Sync sync, void* ctx) { node_ = node; sync_ = sync; sync_ctx_ = ctx; publish(); }
    // Physical ranges that are I/O, for the default mode of map (-1).
    void set_io_test(bool (*is_io)(void*, u32), void* ctx) { is_io_ = is_io; io_ctx_ = ctx; }

    bool open(u32, const char*) override { return true; }
    bool method(u32 slot, const char* name, forth::Forth& f) override;

    bool map(u32 phys, u32 virt, u32 size, u32 mode);  // also for the firmware itself
    void unmap(u32 virt, u32 size);
    bool publish();  // rewrites "translations"

private:
    bool resync();
    DeviceTree& tree_;
    Translations& t_;
    MemoryMap& virt_;
    Phandle node_ = kNone;
    Sync sync_ = nullptr;
    void* sync_ctx_ = nullptr;
    bool (*is_io_)(void*, u32) = nullptr;
    void* io_ctx_ = nullptr;
};

// Methods of /memory: physical claim and release.
class MemoryDriver final : public PackageDriver {
public:
    explicit MemoryDriver(MemoryMap& phys) : phys_(phys) {}
    bool open(u32, const char*) override { return true; }
    bool method(u32 slot, const char* name, forth::Forth& f) override;

private:
    MemoryMap& phys_;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_MMU_H
