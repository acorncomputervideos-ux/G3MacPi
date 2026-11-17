// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/cpu_state.h — architectural state of the PowerPC 750 and
// the constants that describe it.
//
// Sources: "PowerPC Microprocessor Family: The Programming Environments
// Manual for 32-bit Implementations" (PEM) and the "MPC750 RISC
// Microprocessor User's Manual". Bit masks are written with PowerPC bit
// numbering in the comments (bit 0 = most significant).

#ifndef G3_CORE_CPU_PPC750_CPU_STATE_H
#define G3_CORE_CPU_PPC750_CPU_STATE_H

#include "core/common/types.h"

namespace g3::ppc {

// --- Machine State Register (PEM 2.3.1; 750 UM 2.1.3.1) ---------------------
namespace msr {
constexpr u32 POW = 1u << 18;  // bit 13: power management enable
constexpr u32 ILE = 1u << 16;  // bit 15: exception little-endian mode
constexpr u32 EE  = 1u << 15;  // bit 16: external interrupt enable
constexpr u32 PR  = 1u << 14;  // bit 17: problem (user) state
constexpr u32 FP  = 1u << 13;  // bit 18: floating-point available
constexpr u32 ME  = 1u << 12;  // bit 19: machine check enable
constexpr u32 FE0 = 1u << 11;  // bit 20: FP exception mode 0
constexpr u32 SE  = 1u << 10;  // bit 21: single-step trace enable
constexpr u32 BE  = 1u << 9;   // bit 22: branch trace enable
constexpr u32 FE1 = 1u << 8;   // bit 23: FP exception mode 1
constexpr u32 IP  = 1u << 6;   // bit 25: exception prefix (vectors at 0xFFF00000)
constexpr u32 IR  = 1u << 5;   // bit 26: instruction address translation
constexpr u32 DR  = 1u << 4;   // bit 27: data address translation
constexpr u32 PM  = 1u << 2;   // bit 29: performance monitor marked mode (750)
constexpr u32 RI  = 1u << 1;   // bit 30: recoverable exception
constexpr u32 LE  = 1u << 0;   // bit 31: little-endian mode

// MSR bits copied to SRR1 on an exception: bits 16-23, 25-27, 30-31 (PEM 6.4).
constexpr u32 kSrr1CopyMask = 0x0000FF73u;
// MSR bits restored from SRR1 by rfi: bits 0, 5-9, 16-23, 25-27, 30-31 (PEM rfi).
constexpr u32 kRfiMask = 0x87C0FF73u;
} // namespace msr

// --- XER (PEM 2.1.5) ----------------------------------------------------------
namespace xer {
constexpr u32 SO = 0x80000000u;          // bit 0: summary overflow
constexpr u32 OV = 0x40000000u;          // bit 1: overflow
constexpr u32 CA = 0x20000000u;          // bit 2: carry
constexpr u32 kByteCountMask = 0x7Fu;    // bits 25-31: byte count (lswx/stswx)
} // namespace xer

// --- FPSCR (PEM 2.1.4, Table 2-4) ---------------------------------------------
namespace fpscr {
constexpr u32 FX     = 0x80000000u;  // bit 0: exception summary
constexpr u32 FEX    = 0x40000000u;  // bit 1: enabled exception summary
constexpr u32 VX     = 0x20000000u;  // bit 2: invalid operation summary
constexpr u32 OX     = 0x10000000u;  // bit 3: overflow
constexpr u32 UX     = 0x08000000u;  // bit 4: underflow
constexpr u32 ZX     = 0x04000000u;  // bit 5: zero divide
constexpr u32 XX     = 0x02000000u;  // bit 6: inexact
constexpr u32 VXSNAN = 0x01000000u;  // bit 7
constexpr u32 VXISI  = 0x00800000u;  // bit 8: inf - inf
constexpr u32 VXIDI  = 0x00400000u;  // bit 9: inf / inf
constexpr u32 VXZDZ  = 0x00200000u;  // bit 10: 0 / 0
constexpr u32 VXIMZ  = 0x00100000u;  // bit 11: inf * 0
constexpr u32 VXVC   = 0x00080000u;  // bit 12: invalid compare
constexpr u32 FR     = 0x00040000u;  // bit 13: fraction rounded
constexpr u32 FI     = 0x00020000u;  // bit 14: fraction inexact
constexpr u32 FPRF   = 0x0001F000u;  // bits 15-19: result flags (C + FPCC)
constexpr u32 FPCC   = 0x0000F000u;  // bits 16-19: FL FG FE FU
constexpr u32 VXSOFT = 0x00000400u;  // bit 21
constexpr u32 VXSQRT = 0x00000200u;  // bit 22
constexpr u32 VXCVI  = 0x00000100u;  // bit 23: invalid integer convert
constexpr u32 VE     = 0x00000080u;  // bit 24: enables ...
constexpr u32 OE     = 0x00000040u;
constexpr u32 UE     = 0x00000020u;
constexpr u32 ZE     = 0x00000010u;
constexpr u32 XE     = 0x00000008u;
constexpr u32 NI     = 0x00000004u;  // bit 29: non-IEEE mode (not emulated)
constexpr u32 RN     = 0x00000003u;  // bits 30-31: rounding mode

constexpr u32 kAllVx = VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT | VXSQRT | VXCVI;
// Sticky exception bits whose 0 -> 1 transition sets FX.
constexpr u32 kExceptionBits = OX | UX | ZX | XX | kAllVx;

// FPRF result classes (PEM Table 2-5), already shifted into place.
constexpr u32 kClassQNaN    = 0x11u << 12;
constexpr u32 kClassNegInf  = 0x09u << 12;
constexpr u32 kClassNegNorm = 0x08u << 12;
constexpr u32 kClassNegDen  = 0x18u << 12;
constexpr u32 kClassNegZero = 0x12u << 12;
constexpr u32 kClassPosZero = 0x02u << 12;
constexpr u32 kClassPosDen  = 0x14u << 12;
constexpr u32 kClassPosNorm = 0x04u << 12;
constexpr u32 kClassPosInf  = 0x05u << 12;
} // namespace fpscr

// --- MMU (PEM chapter 7) ------------------------------------------------------
namespace mmu {
// Segment register fields (T = 0 form).
constexpr u32 SR_T  = 0x80000000u;   // bit 0: direct-store segment
constexpr u32 SR_KS = 0x40000000u;   // bit 1: supervisor key
constexpr u32 SR_KP = 0x20000000u;   // bit 2: user key
constexpr u32 SR_N  = 0x10000000u;   // bit 3: no-execute
constexpr u32 SR_VSID_MASK = 0x00FFFFFFu;
// PTE word 0 / word 1.
constexpr u32 PTE0_V = 0x80000000u;
constexpr u32 PTE0_H = 0x00000040u;
constexpr u32 PTE1_R = 0x00000100u;
constexpr u32 PTE1_C = 0x00000080u;
constexpr u32 PTE1_G = 0x00000008u;  // WIMG: guarded
// BAT upper/lower.
constexpr u32 BATU_VS = 0x2u;
constexpr u32 BATU_VP = 0x1u;
} // namespace mmu

// DSISR bits for DSI exceptions (PEM Table 6-8).
namespace dsisr {
constexpr u32 NOT_FOUND    = 0x40000000u;  // bit 1: no PTE / BAT
constexpr u32 PROTECTION   = 0x08000000u;  // bit 4
constexpr u32 DIRECT_STORE = 0x04000000u;  // bit 5 (750: T = 1 segment access)
constexpr u32 STORE        = 0x02000000u;  // bit 6
} // namespace dsisr

// SRR1 cause bits for ISI exceptions (PEM 6.4.4).
namespace isi {
constexpr u32 NOT_FOUND  = 0x40000000u;  // bit 1
constexpr u32 NO_EXECUTE = 0x10000000u;  // bit 3: N=1 segment, T=1 segment or guarded page
constexpr u32 PROTECTION = 0x08000000u;  // bit 4
} // namespace isi

// --- Condition register field bits (within a 4-bit field) --------------------
namespace crf {
constexpr u32 LT = 8, GT = 4, EQ = 2, SO = 1;
} // namespace crf

// --- SPR numbers implemented by the 750 (750 UM Table 2-1 / 2-2) ------------
namespace spr {
constexpr u32 XER = 1, LR = 8, CTR = 9;
constexpr u32 DSISR = 18, DAR = 19, DEC = 22, SDR1 = 25, SRR0 = 26, SRR1 = 27;
constexpr u32 TBL_READ = 268, TBU_READ = 269;  // via mftb
constexpr u32 SPRG0 = 272, SPRG1 = 273, SPRG2 = 274, SPRG3 = 275;
constexpr u32 EAR = 282, TBL_WRITE = 284, TBU_WRITE = 285, PVR = 287;
constexpr u32 IBAT0U = 528;  // IBAT0U..IBAT3L = 528..535
constexpr u32 DBAT0U = 536;  // DBAT0U..DBAT3L = 536..543
constexpr u32 UMMCR0 = 936, UPMC1 = 937, UPMC2 = 938, USIA = 939;
constexpr u32 UMMCR1 = 940, UPMC3 = 941, UPMC4 = 942;
constexpr u32 MMCR0 = 952, PMC1 = 953, PMC2 = 954, SIA = 955;
constexpr u32 MMCR1 = 956, PMC3 = 957, PMC4 = 958;
constexpr u32 HID0 = 1008, HID1 = 1009, IABR = 1010, DABR = 1013;
constexpr u32 L2CR = 1017, ICTC = 1019, THRM1 = 1020, THRM2 = 1021, THRM3 = 1022;
constexpr u32 kCount = 1024;
} // namespace spr

// --- Exception vector offsets (PEM Table 6-2; 750 UM Table 4-2) -------------
enum class Exception : u32 {
    SystemReset      = 0x0100,
    MachineCheck     = 0x0200,
    Dsi              = 0x0300,
    Isi              = 0x0400,
    External         = 0x0500,
    Alignment        = 0x0600,
    Program          = 0x0700,
    FpUnavailable    = 0x0800,
    Decrementer      = 0x0900,
    SystemCall       = 0x0C00,
    Trace            = 0x0D00,
    PerfMonitor      = 0x0F00,
    InstrBreakpoint  = 0x1300,
    SystemManagement = 0x1400,
    Thermal          = 0x1700,
};

// SRR1 cause bits for program exceptions (PEM 6.4.7).
namespace srr1 {
constexpr u32 FP_ENABLED = 0x00100000u;  // bit 11
constexpr u32 ILLEGAL    = 0x00080000u;  // bit 12
constexpr u32 PRIVILEGED = 0x00040000u;  // bit 13
constexpr u32 TRAP       = 0x00020000u;  // bit 14
constexpr u32 SUBSEQUENT = 0x00010000u;  // bit 15: SRR0 holds next instruction
} // namespace srr1

// Processor Version Register: the upper half identifies the 750 family
// (0x0008). The revision (lower half) of the Blue & White G3's CPU must come
// from a real machine; 0x0000 here is a placeholder, NOT a verified value.
constexpr u32 kPvrMpc750Placeholder = 0x00080000u;

// Architectural state. Plain data: copying it snapshots the CPU.
struct CpuState {
    u32 gpr[32];
    u64 fpr[32];        // IEEE double bit patterns (FPU: Milestone 2b)
    u32 cr;
    u32 fpscr;
    u32 msr;
    u32 pc;             // address of the instruction being executed / next
    u32 sr[16];         // segment registers
    u32 spr[spr::kCount];  // XER, LR, CTR and all other SPRs, indexed by number
    u64 tb;             // time base (TBU:TBL)

    // lwarx/stwcx. reservation
    bool reservation_valid;
    u32 reservation_addr;

    u32& xer() { return spr[spr::XER]; }
    u32& lr() { return spr[spr::LR]; }
    u32& ctr() { return spr[spr::CTR]; }
    u32 xer() const { return spr[spr::XER]; }
    u32 lr() const { return spr[spr::LR]; }
    u32 ctr() const { return spr[spr::CTR]; }
};

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_CPU_STATE_H
