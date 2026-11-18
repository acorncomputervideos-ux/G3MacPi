// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/guest_view.h — guest memory as the CPU's data
// accesses see it right now. In Virtual-Mode, addresses that pass between
// Open Firmware and its client are virtual (PowerPC binding §4.2), so the
// firmware's Forth and client interface read and write through the CPU's
// translation (MSR[DR], BATs, page table: Ppc750::probe_translate) rather
// than physically. With MSR[DR] = 0 this is the physical bus.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_GUEST_VIEW_H
#define G3_CORE_FIRMWARE_OPENFIRM_GUEST_VIEW_H

#include "core/cpu/ppc750/memory_interface.h"
#include "core/cpu/ppc750/ppc750.h"

namespace g3::of {

class GuestView final : public ppc::MemoryInterface {
public:
    GuestView(ppc::MemoryInterface& phys, ppc::Ppc750& cpu) : phys_(phys), cpu_(cpu) {}

    bool read8(u32 a, u8& v) override;
    bool read16(u32 a, u16& v) override;
    bool read32(u32 a, u32& v) override;
    bool write8(u32 a, u8 v) override;
    bool write16(u32 a, u16 v) override;
    bool write32(u32 a, u32 v) override;

private:
    bool pa(u32 a, bool write, u32& out) { return cpu_.probe_translate(a, write, out); }
    // Accesses that stay within one 4 KB page are translated once; others
    // byte by byte.
    static bool one_page(u32 a, u32 n) { return (a & 0xFFF) + n <= 0x1000; }

    ppc::MemoryInterface& phys_;
    ppc::Ppc750& cpu_;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_GUEST_VIEW_H
