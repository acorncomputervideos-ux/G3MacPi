// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/disasm.cpp — PowerPC 750 disassembler (see disasm.h).

#include "core/cpu/ppc750/disasm.h"

namespace g3::ppc {

namespace {

// ---------------------------------------------------------------------------
// Minimal freestanding text builder.
// ---------------------------------------------------------------------------
class Line {
public:
    void mn(const char* s) { append(mn_, mn_len_, sizeof mn_, s); }
    void mn_char(char c) { char s[2] = {c, 0}; mn(s); }

    void op_sep()
    {
        if (nops_++ > 0)
            append(ops_, ops_len_, sizeof ops_, ", ");
    }
    void op_text(const char* s) { op_sep(); raw(s); }
    void op_reg(char kind, u32 n) { op_sep(); raw_char(kind); raw_dec(n); }
    void op_gpr(u32 n) { op_reg('r', n); }
    void op_fpr(u32 n) { op_reg('f', n); }
    void op_gpr_or_zero(u32 n) { if (n == 0) op_text("0"); else op_gpr(n); }
    void op_dec(u32 v) { op_sep(); raw_dec(v); }
    void op_hex(u32 v) { op_sep(); raw_hex(v, 1); }
    void op_hex2(u32 v) { op_sep(); raw_hex(v, 2); }
    void op_shex(s32 v) { op_sep(); raw_shex(v); }
    void op_addr(u32 v) { op_sep(); raw_hex(v, 8); }
    void op_crf(u32 n) { op_sep(); raw("cr"); raw_dec(n); }
    void op_crbit(u32 bit)
    {
        static const char* const kNames[4] = {"lt", "gt", "eq", "so"};
        op_sep();
        if (bit >= 4) {
            raw("4*cr");
            raw_dec(bit >> 2);
            raw_char('+');
        }
        raw(kNames[bit & 3]);
    }
    // "d(rA)" or just "d" when rA = 0.
    void op_disp(s32 d, u32 ra)
    {
        op_sep();
        raw_shex(d);
        if (ra != 0) {
            raw("(r");
            raw_dec(ra);
            raw_char(')');
        }
    }

    void emit(char* out, size_t size) const
    {
        size_t n = 0;
        auto put = [&](char c) {
            if (n + 1 < size)
                out[n++] = c;
        };
        for (size_t i = 0; i < mn_len_; ++i)
            put(mn_[i]);
        if (nops_ > 0) {
            size_t pad = mn_len_ < 8 ? 8 - mn_len_ : 1;
            while (pad--)
                put(' ');
            for (size_t i = 0; i < ops_len_; ++i)
                put(ops_[i]);
        }
        if (size > 0)
            out[n] = 0;
    }

private:
    static void append(char* buf, size_t& len, size_t cap, const char* s)
    {
        while (*s && len + 1 < cap)
            buf[len++] = *s++;
    }
    void raw(const char* s) { append(ops_, ops_len_, sizeof ops_, s); }
    void raw_char(char c) { char s[2] = {c, 0}; raw(s); }
    void raw_dec(u32 v)
    {
        char tmp[11];
        int i = 0;
        do {
            tmp[i++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (i)
            raw_char(tmp[--i]);
    }
    void raw_hex(u32 v, int min_digits)
    {
        static const char kDigits[] = "0123456789ABCDEF";
        char tmp[8];
        int i = 0;
        do {
            tmp[i++] = kDigits[v & 0xF];
            v >>= 4;
        } while (v || i < min_digits);
        raw("0x");
        while (i)
            raw_char(tmp[--i]);
    }
    void raw_shex(s32 v)
    {
        if (v < 0) {
            raw_char('-');
            raw_hex(0u - static_cast<u32>(v), 1);
        } else {
            raw_hex(static_cast<u32>(v), 1);
        }
    }

    char mn_[24] = {};
    size_t mn_len_ = 0;
    char ops_[96] = {};
    size_t ops_len_ = 0;
    int nops_ = 0;
};

// Instruction fields (PowerPC bit numbering, PEM Appendix A).
constexpr u32 OPCD(u32 i) { return i >> 26; }
constexpr u32 RD(u32 i) { return (i >> 21) & 31; }
constexpr u32 RA(u32 i) { return (i >> 16) & 31; }
constexpr u32 RB(u32 i) { return (i >> 11) & 31; }
constexpr u32 RC_(u32 i) { return (i >> 6) & 31; }   // frC
constexpr u32 MB(u32 i) { return (i >> 6) & 31; }
constexpr u32 ME(u32 i) { return (i >> 1) & 31; }
constexpr u32 XO10(u32 i) { return (i >> 1) & 0x3FF; }
constexpr u32 XO9(u32 i) { return (i >> 1) & 0x1FF; }
constexpr u32 XO5(u32 i) { return (i >> 1) & 0x1F; }
constexpr bool RCBIT(u32 i) { return (i & 1) != 0; }
constexpr bool OEBIT(u32 i) { return (i & 0x400) != 0; }
constexpr s32 SIMM(u32 i) { return static_cast<s32>(static_cast<s16>(i & 0xFFFF)); }
constexpr u32 UIMM(u32 i) { return i & 0xFFFF; }
constexpr u32 CRFD(u32 i) { return (i >> 23) & 7; }
constexpr u32 CRFS(u32 i) { return (i >> 18) & 7; }

// Mnemonic with optional "o" (OE) and "." (Rc) suffixes.
void mn_oe_rc(Line& l, const char* base, bool oe, bool rc)
{
    l.mn(base);
    if (oe) l.mn_char('o');
    if (rc) l.mn_char('.');
}

void mn_rc(Line& l, const char* base, bool rc) { mn_oe_rc(l, base, false, rc); }

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------
enum class BranchKind { Rel, Lr, Ctr };

bool branch(Line& l, u32 addr, u32 insn, BranchKind kind)
{
    const u32 bo = RD(insn);
    const u32 bi = RA(insn);
    const bool lk = (insn & 1) != 0;
    const bool aa = kind == BranchKind::Rel && (insn & 2) != 0;
    static const char* const kTrue[4] = {"lt", "gt", "eq", "so"};
    static const char* const kFalse[4] = {"ge", "le", "ne", "ns"};

    const bool decrement = !(bo & 0x04);
    const bool always = (bo & 0x10) && (bo & 0x04);
    if (kind == BranchKind::Ctr && decrement)
        return false;  // bcctr with CTR decrement: invalid form

    l.mn("b");
    bool cr_field_operand = false, cr_bit_operand = false;
    if (always) {
        // nothing: "b", "blr", "bctr"
    } else if (!decrement) {
        l.mn((bo & 0x08) ? kTrue[bi & 3] : kFalse[bi & 3]);
        cr_field_operand = (bi >> 2) != 0;
    } else {
        l.mn((bo & 0x02) ? "dz" : "dnz");
        if (!(bo & 0x10)) {
            l.mn((bo & 0x08) ? "t" : "f");
            cr_bit_operand = true;
        }
    }
    if (kind == BranchKind::Lr) l.mn("lr");
    if (kind == BranchKind::Ctr) l.mn("ctr");
    if (lk) l.mn_char('l');
    if (aa) l.mn_char('a');

    const s32 bd = static_cast<s32>(static_cast<s16>(insn & 0xFFFC));
    if (!always && (bo & 1)) {
        // y bit: for bc it reverses the default (backward = taken)
        // prediction; for bclr/bcctr it means "predict taken".
        l.mn((kind == BranchKind::Rel && bd < 0) ? "-" : "+");
    }

    if (cr_field_operand)
        l.op_crf(bi >> 2);
    if (cr_bit_operand)
        l.op_crbit(bi);
    if (kind == BranchKind::Rel)
        l.op_addr(aa ? static_cast<u32>(bd) : addr + static_cast<u32>(bd));
    return true;
}

// ---------------------------------------------------------------------------
// Rotates (rlwinm / rlwnm / rlwimi) with simplified mnemonics
// ---------------------------------------------------------------------------
void rotate(Line& l, u32 insn, u32 op)
{
    const u32 rs = RD(insn), ra = RA(insn), sh = RB(insn), mb = MB(insn), me = ME(insn);
    const bool rc = RCBIT(insn);

    if (op == 23) {  // rlwnm
        if (mb == 0 && me == 31) {
            mn_rc(l, "rotlw", rc);
            l.op_gpr(ra); l.op_gpr(rs); l.op_gpr(sh);
        } else {
            mn_rc(l, "rlwnm", rc);
            l.op_gpr(ra); l.op_gpr(rs); l.op_gpr(sh); l.op_dec(mb); l.op_dec(me);
        }
        return;
    }

    if (op == 20) {  // rlwimi
        if (mb <= me) {
            const u32 n = me - mb + 1, b = mb;
            if (b >= 1 && sh == 32 - b) {
                mn_rc(l, "inslwi", rc);
                l.op_gpr(ra); l.op_gpr(rs); l.op_dec(n); l.op_dec(b);
                return;
            }
            if (b + n <= 32 && sh == ((32 - (b + n)) & 31)) {
                mn_rc(l, "insrwi", rc);
                l.op_gpr(ra); l.op_gpr(rs); l.op_dec(n); l.op_dec(b);
                return;
            }
        }
        mn_rc(l, "rlwimi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(sh); l.op_dec(mb); l.op_dec(me);
        return;
    }

    // rlwinm — order matters (derived from the reference vectors).
    if (mb == 0 && me == 31) {
        if (sh < 16) {
            mn_rc(l, "rotlwi", rc);
            l.op_gpr(ra); l.op_gpr(rs); l.op_dec(sh);
        } else {
            mn_rc(l, "rotrwi", rc);
            l.op_gpr(ra); l.op_gpr(rs); l.op_dec(32 - sh);
        }
    } else if (mb == 0 && me == 31 - sh) {
        mn_rc(l, "slwi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(sh);
    } else if (me == 31 && sh != 0 && sh + mb == 32) {
        mn_rc(l, "srwi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(mb);
    } else if (sh == 0 && me == 31) {
        mn_rc(l, "clrlwi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(mb);
    } else if (sh == 0 && mb == 0) {
        mn_rc(l, "clrrwi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(31 - me);
    } else if (mb == 0) {
        mn_rc(l, "extlwi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(me + 1); l.op_dec(sh);
    } else if (me == 31 && sh >= 32 - mb) {
        const u32 n = 32 - mb;
        mn_rc(l, "extrwi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(n); l.op_dec(sh - n);
    } else if (me == 31 - sh && mb + sh < 32) {
        mn_rc(l, "clrlslwi", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(mb + sh); l.op_dec(sh);
    } else {
        mn_rc(l, "rlwinm", rc);
        l.op_gpr(ra); l.op_gpr(rs); l.op_dec(sh); l.op_dec(mb); l.op_dec(me);
    }
}

// ---------------------------------------------------------------------------
// SPR names
// ---------------------------------------------------------------------------
const char* spr_name(u32 n)
{
    switch (n) {
    case 1: return "XER";
    case 8: return "LR";
    case 9: return "CTR";
    case 18: return "DSISR";
    case 19: return "DAR";
    case 22: return "DEC";
    case 25: return "SDR1";
    case 26: return "SRR0";
    case 27: return "SRR1";
    case 272: return "SPRG0";
    case 273: return "SPRG1";
    case 274: return "SPRG2";
    case 275: return "SPRG3";
    case 282: return "EAR";
    case 284: return "TBL";
    case 285: return "TBU";
    case 287: return "PVR";
    case 528: return "IBAT0U";
    case 529: return "IBAT0L";
    case 530: return "IBAT1U";
    case 531: return "IBAT1L";
    case 532: return "IBAT2U";
    case 533: return "IBAT2L";
    case 534: return "IBAT3U";
    case 535: return "IBAT3L";
    case 536: return "DBAT0U";
    case 537: return "DBAT0L";
    case 538: return "DBAT1U";
    case 539: return "DBAT1L";
    case 540: return "DBAT2U";
    case 541: return "DBAT2L";
    case 542: return "DBAT3U";
    case 543: return "DBAT3L";
    case 936: return "UMMCR0";
    case 937: return "UPMC1";
    case 938: return "UPMC2";
    case 939: return "USIA";
    case 940: return "UMMCR1";
    case 941: return "UPMC3";
    case 942: return "UPMC4";
    case 952: return "MMCR0";
    case 953: return "PMC1";
    case 954: return "PMC2";
    case 955: return "SIA";
    case 956: return "MMCR1";
    case 957: return "PMC3";
    case 958: return "PMC4";
    case 1008: return "HID0";
    case 1009: return "HID1";
    case 1010: return "IABR";
    case 1013: return "DABR";
    case 1017: return "L2CR";
    case 1019: return "ICTC";
    case 1020: return "THRM1";
    case 1021: return "THRM2";
    case 1022: return "THRM3";
    default: return nullptr;
    }
}

void op_spr(Line& l, u32 n)
{
    l.op_text(spr_name(n));  // only called for implemented SPRs
}

// ---------------------------------------------------------------------------
// Opcode 31
// ---------------------------------------------------------------------------
bool op31(Line& l, u32 insn)
{
    const u32 rd = RD(insn), ra = RA(insn), rb = RB(insn);
    const bool rc = RCBIT(insn), oe = OEBIT(insn);

    // XO-form arithmetic: rD, rA, rB (OE, Rc allowed)
    switch (XO9(insn)) {
    case 266: case 10: case 138: case 40: case 8: case 136: case 235: case 491: case 459: {
        const char* name = nullptr;
        switch (XO9(insn)) {
        case 266: name = "add"; break;
        case 10:  name = "addc"; break;
        case 138: name = "adde"; break;
        case 40:  name = "subf"; break;
        case 8:   name = "subfc"; break;
        case 136: name = "subfe"; break;
        case 235: name = "mullw"; break;
        case 491: name = "divw"; break;
        case 459: name = "divwu"; break;
        }
        mn_oe_rc(l, name, oe, rc);
        l.op_gpr(rd); l.op_gpr(ra); l.op_gpr(rb);
        return true;
    }
    case 234: case 202: case 232: case 200: case 104: {  // rD, rA; rB must be 0
        if (rb != 0)
            return false;
        const char* name = nullptr;
        switch (XO9(insn)) {
        case 234: name = "addme"; break;
        case 202: name = "addze"; break;
        case 232: name = "subfme"; break;
        case 200: name = "subfze"; break;
        case 104: name = "neg"; break;
        }
        mn_oe_rc(l, name, oe, rc);
        l.op_gpr(rd); l.op_gpr(ra);
        return true;
    }
    default:
        break;
    }

    const u32 xo = XO10(insn);
    switch (xo) {
    case 11: case 75:  // mulhwu, mulhw (bit 21 reserved)
        mn_rc(l, xo == 11 ? "mulhwu" : "mulhw", rc);
        l.op_gpr(rd); l.op_gpr(ra); l.op_gpr(rb);
        return true;

    // Logical rA, rS, rB
    case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:
    case 24: case 536: case 792: {
        if (xo == 444 && rd == rb) {
            mn_rc(l, "mr", rc);
            l.op_gpr(ra); l.op_gpr(rd);
            return true;
        }
        const char* name = nullptr;
        switch (xo) {
        case 28:  name = "and"; break;
        case 60:  name = "andc"; break;
        case 124: name = "nor"; break;
        case 284: name = "eqv"; break;
        case 316: name = "xor"; break;
        case 412: name = "orc"; break;
        case 444: name = "or"; break;
        case 476: name = "nand"; break;
        case 24:  name = "slw"; break;
        case 536: name = "srw"; break;
        case 792: name = "sraw"; break;
        }
        mn_rc(l, name, rc);
        l.op_gpr(ra); l.op_gpr(rd); l.op_gpr(rb);
        return true;
    }
    case 26: case 922: case 954:  // cntlzw extsh extsb: rA, rS
        if (rb != 0)
            return false;
        mn_rc(l, xo == 26 ? "cntlzw" : (xo == 922 ? "extsh" : "extsb"), rc);
        l.op_gpr(ra); l.op_gpr(rd);
        return true;
    case 824:  // srawi
        mn_rc(l, "srawi", rc);
        l.op_gpr(ra); l.op_gpr(rd); l.op_hex(rb);
        return true;

    case 0: case 32: {  // cmp, cmpl
        if (rc || (insn & 0x00400000u))
            return false;
        const u32 crf = CRFD(insn);
        const bool lbit = (insn & 0x00200000u) != 0;
        if (lbit) {
            l.mn(xo == 0 ? "cmp" : "cmpl");
            l.op_crf(crf);
        } else {
            l.mn(xo == 0 ? "cmpw" : "cmplw");
            if (crf) l.op_crf(crf);
        }
        l.op_gpr(ra); l.op_gpr(rb);
        return true;
    }
    case 4:  // tw
        if (rc)
            return false;
        l.mn("tw");
        l.op_dec(rd); l.op_gpr(ra); l.op_gpr(rb);
        return true;

    case 19:  // mfcr
        if (rc || ra || rb) return false;
        l.mn("mfcr"); l.op_gpr(rd);
        return true;
    case 144: {  // mtcrf
        if (rc || (insn & 0x00100800u)) return false;
        const u32 crm = (insn >> 12) & 0xFF;
        if (crm == 0xFF) {
            l.mn("mtcr"); l.op_gpr(rd);
        } else {
            l.mn("mtcrf"); l.op_hex2(crm); l.op_gpr(rd);
        }
        return true;
    }
    case 512:  // mcrxr
        if (rc || (insn & 0x007FF800u)) return false;
        l.mn("mcrxr"); l.op_crf(CRFD(insn));
        return true;
    case 83:  // mfmsr
        if (rc || ra || rb) return false;
        l.mn("mfmsr"); l.op_gpr(rd);
        return true;
    case 146:  // mtmsr
        if (rc || ra || rb) return false;
        l.mn("mtmsr"); l.op_gpr(rd);
        return true;
    case 339: case 467: case 371: {  // mfspr, mtspr, mftb
        if (rc) return false;
        const u32 n = ra | (rb << 5);
        if (xo == 371) {
            if (n == 268) { l.mn("mftb"); l.op_gpr(rd); return true; }
            if (n == 269) { l.mn("mftbu"); l.op_gpr(rd); return true; }
            return false;
        }
        const bool from = xo == 339;
        // Same validity rules as the interpreter (ppc750_system.cpp): only
        // SPRs the 750 implements; TBL/TBU (284/285) are write-only; the
        // user copies of the performance monitor registers are read-only.
        if (!spr_name(n) || (from && (n == 284 || n == 285)) || (!from && n >= 936 && n <= 942))
            return false;
        const char* simple = n == 1 ? "xer" : (n == 8 ? "lr" : (n == 9 ? "ctr" : nullptr));
        if (simple) {
            l.mn(from ? "mf" : "mt");
            l.mn(simple);
            l.op_gpr(rd);
        } else if (from) {
            l.mn("mfspr"); l.op_gpr(rd); op_spr(l, n);
        } else {
            l.mn("mtspr"); op_spr(l, n); l.op_gpr(rd);
        }
        return true;
    }
    case 595: case 210:  // mfsr, mtsr
        if (rc || rb || (insn & 0x00100000u)) return false;
        if (xo == 595) { l.mn("mfsr"); l.op_gpr(rd); l.op_dec(ra & 0xF); }
        else { l.mn("mtsr"); l.op_dec(ra & 0xF); l.op_gpr(rd); }
        return true;
    case 659: case 242:  // mfsrin, mtsrin
        if (rc || ra) return false;
        l.mn(xo == 659 ? "mfsrin" : "mtsrin"); l.op_gpr(rd); l.op_gpr(rb);
        return true;
    case 306:  // tlbie
        if (rc || rd || ra) return false;
        l.mn("tlbie"); l.op_gpr(rb);
        return true;
    case 566: case 598: case 854:  // tlbsync sync eieio
        if ((insn & 0x03FFF801u) != 0) return false;
        l.mn(xo == 566 ? "tlbsync" : (xo == 598 ? "sync" : "eieio"));
        return true;

    case 54: case 86: case 246: case 278: case 470: case 982: case 1014: {  // cache ops
        if (rc || rd) return false;
        const char* name = xo == 54 ? "dcbst" : xo == 86 ? "dcbf" : xo == 246 ? "dcbtst"
                         : xo == 278 ? "dcbt" : xo == 470 ? "dcbi" : xo == 982 ? "icbi" : "dcbz";
        l.mn(name); l.op_gpr_or_zero(ra); l.op_gpr(rb);
        return true;
    }

    case 20:  // lwarx
        if (rc) return false;
        l.mn("lwarx"); l.op_gpr(rd); l.op_gpr_or_zero(ra); l.op_gpr(rb);
        return true;
    case 150:  // stwcx. (Rc must be 1)
        if (!rc) return false;
        l.mn("stwcx."); l.op_gpr(rd); l.op_gpr_or_zero(ra); l.op_gpr(rb);
        return true;
    case 597: case 725:  // lswi, stswi
        if (rc) return false;
        l.mn(xo == 597 ? "lswi" : "stswi");
        l.op_gpr(rd); l.op_gpr_or_zero(ra); l.op_hex(rb == 0 ? 32 : rb);
        return true;
    case 533: case 661:  // lswx, stswx
        if (rc) return false;
        l.mn(xo == 533 ? "lswx" : "stswx");
        l.op_gpr(rd); l.op_gpr_or_zero(ra); l.op_gpr(rb);
        return true;
    default:
        break;
    }

    // Indexed loads/stores: first operand rD/rS or frD/frS.
    struct Ix { u32 xo; const char* name; bool fp; bool update; };
    static const Ix kIndexed[] = {
        {23, "lwzx", false, false},   {55, "lwzux", false, true},  {87, "lbzx", false, false},
        {119, "lbzux", false, true},  {151, "stwx", false, false}, {183, "stwux", false, true},
        {215, "stbx", false, false},  {247, "stbux", false, true}, {279, "lhzx", false, false},
        {311, "lhzux", false, true},  {343, "lhax", false, false}, {375, "lhaux", false, true},
        {407, "sthx", false, false},  {439, "sthux", false, true}, {534, "lwbrx", false, false},
        {790, "lhbrx", false, false}, {662, "stwbrx", false, false}, {918, "sthbrx", false, false},
        {535, "lfsx", true, false},   {567, "lfsux", true, true},  {599, "lfdx", true, false},
        {631, "lfdux", true, true},   {663, "stfsx", true, false}, {695, "stfsux", true, true},
        {727, "stfdx", true, false},  {759, "stfdux", true, true}, {983, "stfiwx", true, false},
    };
    for (const Ix& e : kIndexed) {
        if (e.xo != xo)
            continue;
        if (rc)
            return false;
        l.mn(e.name);
        if (e.fp) l.op_fpr(rd); else l.op_gpr(rd);
        if (e.update) l.op_gpr(ra); else l.op_gpr_or_zero(ra);
        l.op_gpr(rb);
        return true;
    }
    return false;  // includes tlbia, eciwx/ecowx and POWER-only opcodes
}

// ---------------------------------------------------------------------------
// Floating point (opcodes 59 and 63)
// ---------------------------------------------------------------------------
bool fp_arith(Line& l, u32 insn, bool single)
{
    const u32 xo = XO5(insn), d = RD(insn), a = RA(insn), b = RB(insn), c = RC_(insn);
    const bool rc = RCBIT(insn);
    const char* name = nullptr;
    enum { AB, AC, ACB, B } form = AB;
    switch (xo) {
    case 18: name = single ? "fdivs" : "fdiv"; break;
    case 20: name = single ? "fsubs" : "fsub"; break;
    case 21: name = single ? "fadds" : "fadd"; break;
    case 25: name = single ? "fmuls" : "fmul"; form = AC; break;
    case 28: name = single ? "fmsubs" : "fmsub"; form = ACB; break;
    case 29: name = single ? "fmadds" : "fmadd"; form = ACB; break;
    case 30: name = single ? "fnmsubs" : "fnmsub"; form = ACB; break;
    case 31: name = single ? "fnmadds" : "fnmadd"; form = ACB; break;
    case 23: if (single) return false; name = "fsel"; form = ACB; break;
    case 24: if (!single) return false; name = "fres"; form = B; break;
    case 26: if (single) return false; name = "frsqrte"; form = B; break;
    default: return false;  // includes fsqrt/fsqrts: not on the 750
    }
    if ((form == AB && c) || (form == AC && b) || (form == B && (a || c)))
        return false;
    mn_rc(l, name, rc);
    l.op_fpr(d);
    switch (form) {
    case AB:  l.op_fpr(a); l.op_fpr(b); break;
    case AC:  l.op_fpr(a); l.op_fpr(c); break;
    case ACB: l.op_fpr(a); l.op_fpr(c); l.op_fpr(b); break;
    case B:   l.op_fpr(b); break;
    }
    return true;
}

bool op63_x(Line& l, u32 insn)
{
    const u32 d = RD(insn), a = RA(insn), b = RB(insn);
    const bool rc = RCBIT(insn);
    switch (XO10(insn)) {
    case 0: case 32:  // fcmpu, fcmpo
        if (rc || (insn & 0x00600000u)) return false;
        l.mn(XO10(insn) == 0 ? "fcmpu" : "fcmpo");
        l.op_crf(CRFD(insn)); l.op_fpr(a); l.op_fpr(b);
        return true;
    case 12: case 14: case 15: case 40: case 72: case 136: case 264: {
        if (a) return false;
        const u32 xo = XO10(insn);
        mn_rc(l, xo == 12 ? "frsp" : xo == 14 ? "fctiw" : xo == 15 ? "fctiwz" : xo == 40 ? "fneg"
                 : xo == 72 ? "fmr" : xo == 136 ? "fnabs" : "fabs", rc);
        l.op_fpr(d); l.op_fpr(b);
        return true;
    }
    case 583:  // mffs
        if (a || b) return false;
        mn_rc(l, "mffs", rc); l.op_fpr(d);
        return true;
    case 711:  // mtfsf
        if (insn & 0x02010000u) return false;
        mn_rc(l, "mtfsf", rc); l.op_dec((insn >> 17) & 0xFF); l.op_fpr(b);
        return true;
    case 134:  // mtfsfi
        if (insn & 0x007F0800u) return false;
        mn_rc(l, "mtfsfi", rc); l.op_crf(CRFD(insn)); l.op_dec((insn >> 12) & 0xF);
        return true;
    case 38: case 70:  // mtfsb1, mtfsb0
        if (a || b) return false;
        mn_rc(l, XO10(insn) == 38 ? "mtfsb1" : "mtfsb0", rc); l.op_dec(d);
        return true;
    case 64:  // mcrfs
        if (rc || (insn & 0x0063F800u)) return false;
        l.mn("mcrfs"); l.op_crf(CRFD(insn)); l.op_crf(CRFS(insn));
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Top level
// ---------------------------------------------------------------------------
bool decode(Line& l, u32 addr, u32 insn)
{
    const u32 op = OPCD(insn);
    const u32 rd = RD(insn), ra = RA(insn);

    switch (op) {
    case 3:  // twi
        l.mn("twi"); l.op_dec(rd); l.op_gpr(ra); l.op_shex(SIMM(insn));
        return true;
    case 7: case 8: case 12: case 13:
        l.mn(op == 7 ? "mulli" : op == 8 ? "subfic" : op == 12 ? "addic" : "addic.");
        l.op_gpr(rd); l.op_gpr(ra); l.op_shex(SIMM(insn));
        return true;
    case 14:
        if (ra == 0) { l.mn("li"); l.op_gpr(rd); }
        else { l.mn("addi"); l.op_gpr(rd); l.op_gpr(ra); }
        l.op_shex(SIMM(insn));
        return true;
    case 15:
        if (ra == 0) { l.mn("lis"); l.op_gpr(rd); }
        else { l.mn("addis"); l.op_gpr(rd); l.op_gpr(ra); }
        l.op_shex(SIMM(insn));
        return true;
    case 10: case 11: {  // cmpli, cmpi
        if (insn & 0x00400000u) return false;
        const u32 crf = CRFD(insn);
        const bool lbit = (insn & 0x00200000u) != 0;
        if (lbit) {
            l.mn(op == 10 ? "cmpli" : "cmpi");
            l.op_crf(crf);
        } else {
            l.mn(op == 10 ? "cmplwi" : "cmpwi");
            if (crf) l.op_crf(crf);
        }
        l.op_gpr(ra);
        if (op == 10) l.op_hex(UIMM(insn)); else l.op_shex(SIMM(insn));
        return true;
    }
    case 16:
        return branch(l, addr, insn, BranchKind::Rel);
    case 17:
        if (insn != 0x44000002u) return false;
        l.mn("sc");
        return true;
    case 18: {
        const s32 li = (static_cast<s32>(insn << 6) >> 6) & ~3;  // 26-bit signed
        const bool aa = (insn & 2) != 0, lk = (insn & 1) != 0;
        l.mn("b");
        if (lk) l.mn_char('l');
        if (aa) l.mn_char('a');
        l.op_addr(aa ? static_cast<u32>(li) : addr + static_cast<u32>(li));
        return true;
    }
    case 19:
        switch (XO10(insn)) {
        case 0:  // mcrf
            if (insn & 0x0063F801u) return false;
            l.mn("mcrf"); l.op_crf(CRFD(insn)); l.op_crf(CRFS(insn));
            return true;
        case 16:
            if (RB(insn)) return false;
            return branch(l, addr, insn, BranchKind::Lr);
        case 528:
            if (RB(insn)) return false;
            return branch(l, addr, insn, BranchKind::Ctr);
        case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: {
            if (insn & 1) return false;
            const u32 xo = XO10(insn), bt = rd, ba = ra, bb = RB(insn);
            if (xo == 193 && bt == ba && ba == bb) { l.mn("crclr"); l.op_crbit(bt); return true; }
            if (xo == 289 && bt == ba && ba == bb) { l.mn("crset"); l.op_crbit(bt); return true; }
            if (xo == 449 && ba == bb) { l.mn("crmove"); l.op_crbit(bt); l.op_crbit(ba); return true; }
            if (xo == 33 && ba == bb) { l.mn("crnot"); l.op_crbit(bt); l.op_crbit(ba); return true; }
            l.mn(xo == 33 ? "crnor" : xo == 129 ? "crandc" : xo == 193 ? "crxor" : xo == 225 ? "crnand"
                 : xo == 257 ? "crand" : xo == 289 ? "creqv" : xo == 417 ? "crorc" : "cror");
            l.op_crbit(bt); l.op_crbit(ba); l.op_crbit(bb);
            return true;
        }
        case 50:
            if (insn != 0x4C000064u) return false;
            l.mn("rfi");
            return true;
        case 150:
            if (insn != 0x4C00012Cu) return false;
            l.mn("isync");
            return true;
        default:
            return false;
        }
    case 20: case 21: case 23:
        rotate(l, insn, op);
        return true;
    case 24:
        if (insn == 0x60000000u) { l.mn("nop"); return true; }
        [[fallthrough]];
    case 25: case 26: case 27: case 28: case 29: {
        static const char* const kNames[6] = {"ori", "oris", "xori", "xoris", "andi.", "andis."};
        l.mn(kNames[op - 24]);
        l.op_gpr(ra); l.op_gpr(rd); l.op_hex(UIMM(insn));
        return true;
    }
    case 31:
        return op31(l, insn);
    case 59:
        return fp_arith(l, insn, true);
    case 63:
        if (XO5(insn) >= 16)
            return fp_arith(l, insn, false);
        return op63_x(l, insn);
    default:
        break;
    }

    if (op >= 32 && op <= 55) {
        static const char* const kNames[24] = {
            "lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu", "lhz", "lhzu", "lha", "lhau",
            "sth", "sthu", "lmw", "stmw", "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu"};
        l.mn(kNames[op - 32]);
        if (op >= 48) l.op_fpr(rd); else l.op_gpr(rd);
        l.op_disp(SIMM(insn), ra);
        return true;
    }
    return false;
}

} // namespace

bool disassemble(u32 addr, u32 insn, char* out, size_t out_size)
{
    Line l;
    if (decode(l, addr, insn)) {
        l.emit(out, out_size);
        return true;
    }
    Line bad;  // "dc.l 0xXXXXXXXX" (always 8 hex digits)
    bad.mn("dc.l");
    bad.op_addr(insn);
    bad.emit(out, out_size);
    return false;
}

} // namespace g3::ppc
