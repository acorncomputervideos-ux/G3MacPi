// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/davbus.h — the sound cell of Paddington, the "DAV
// bus" controller, with its codec and its sample stream on two DBDMA
// channels. Two codecs are modelled: Screamer (AWACS family) and Burgundy.
// A real Blue & White G3 very likely has Burgundy (FreeBSD's davbus.c gives
// "B&W G3 wirings" in its Burgundy code; DingusPPC puts Screamer in the
// Beige G3), but Mac OS 9.2.1 drove the codec with the Screamer protocol
// when the device tree named Burgundy as well (seen in a trace; it then
// waited for Screamer's "valid" bit for ever), so the machine uses Screamer.
//
// Facts (docs/hardware/davbus.md):
//  - Registers at mac-io + 0x14000 (DingusPPC MacIoTwo), 32-bit
//    little-endian, 16 bytes apart (Linux sound/ppc/awacs.h struct
//    awacs_regs): 0x00 sound control, 0x10 codec control, 0x20 codec
//    status, 0x30 clip count, 0x40 byte swap (1 = little-endian samples);
//    0x50 frame count (DingusPPC). Mac OS 9.2.1 reads the frame count with
//    DBDMA LOAD_WORD commands placed before and after its output buffers
//    (seen in a trace) and starts sound only if it advanced as the frames
//    went out.
//  - Sound control: bits 8-10 select the rate, 44100 / 29400 / 22050 /
//    17640 / 14700 / 11025 / 8820 / 7350 Hz; bit 11 error and bit 12 port
//    change are cleared by writing 1; bits 13/14 enable their interrupts.
//  - Codec control: bits 0-11 data, bits 12-14 codec register; bit 24
//    "busy" until the codec took the word. Codec register 1: bit 7 mutes
//    output C (speaker), bit 9 output A (headphone/line); registers 2 (A)
//    and 4 (C): attenuation, right in bits 0-3, left in bits 6-9.
//  - Codec status: bit 22 valid, bits 12-15 revision (3: Screamer), bits
//    8-11 manufacturer (1: Crystal), bits 0-3 jack sense (Linux awacs.h
//    and awacs.c; DingusPPC places these fields differently, see the doc).
//  - Burgundy (Linux sound/ppc/burgundy.c, burgundy.h): registers of 1 or
//    4 bytes at 8-bit addresses, reached through codec control: bits
//    12-19 the address, bits 20-21 the operation (1 read, 2 write one byte
//    of a 4-byte register, 3 write a 1-byte register), bits 8-9 the byte,
//    bits 0-7 the data. A read's byte comes back in codec status bits
//    4-11; codec status bit 23 ("extend") is set, then clears, as it
//    arrives. Register 0x60 enables outputs (0x80 internal speaker, 0x06
//    output, 0x18 line out, 0x60 headphones). Its volume registers are
//    kept but not applied (their scale is not established).
//  - Samples: 16-bit signed stereo, big-endian unless byte swap is 1.
//  - DMA: output on the channel at mac-io + 0x8800, input at + 0x8900;
//    interrupts 17 (sound), 8 (out), 9 (in) — a real B&W's Linux log and
//    DingusPPC agree.
//
// Timing: the codec clocks frames at its rate in guest time; the frame
// count follows the clock. Every kTickFrames frames the frames of that
// interval are taken from the output DMA through a small FIFO (so DMA runs
// at most a FIFO ahead of the codec) and go to the host (AudioSink). Input
// delivers silence at the same rate (there is no microphone).
//
// Not verified: the attenuation step (1.5 dB is assumed) and how Mac OS
// itself drives this codec; see the doc.

#ifndef G3_CORE_DEVICES_MACIO_DAVBUS_H
#define G3_CORE_DEVICES_MACIO_DAVBUS_H

#include "core/common/types.h"
#include "core/devices/macio/dbdma.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"

namespace g3 {

// Where the machine's sound goes: interleaved left/right samples.
class AudioSink {
public:
    virtual void audio_out(const s16* frames, u32 count, u32 rate_hz) = 0;

protected:
    ~AudioSink() = default;
};

class MacioDavbus final : public MmioDevice {
public:
    static constexpr u32 kWindowSize = 0x1000;
    static constexpr u32 kTickFrames = 32;  // 0.73 ms at 44.1 kHz
    static constexpr u32 kFifoBytes = 64;   // 16 frames
    enum Reg : u32 { CONTROL = 0x00, CODEC_CTRL = 0x10, CODEC_STAT = 0x20, CLIP = 0x30, BYTESWAP = 0x40,
                     FRAMES = 0x50 };
    static constexpr u32 kCodecStatus = (1u << 22) | (3u << 12) | (1u << 8);
    static constexpr u32 kRates[8] = {44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350};
    enum class Codec : u8 { Screamer, Burgundy };
    static constexpr u32 kBurgundyOutputs = 0x60;  // MORE_OUTPUTENABLES

    explicit MacioDavbus(Scheduler& scheduler, Codec codec = Codec::Screamer);
    Codec codec() const { return codec_type_; }
    void set_codec(Codec c) { codec_type_ = c; }
    u8 burgundy_reg(u32 addr, u32 byte) const { return breg_[addr & 0xFF][byte & 3]; }

    void attach_dma(DbdmaChannel* out, DbdmaChannel* in);
    void set_sink(AudioSink* sink) { sink_ = sink; }

    u32 rate() const { return kRates[(control_ >> 8) & 7]; }
    u32 codec_reg(u32 n) const { return codec_[n & 7]; }
    u64 frames_played() const { return frames_; }  // taken from the output DMA
    u32 frame_count() const;                         // the register: the codec's clock
    // Gain applied to the output, Q15 per side (0 when muted).
    void gains(u32& left, u32& right) const;

    // MmioDevice.
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    struct Out final : DbdmaStream {
        MacioDavbus* s = nullptr;
        u32 dma_output(const u8* data, u32 len, bool last) override;
        u32 dma_input(u8*, u32) override { return 0; }
    };
    struct In final : DbdmaStream {
        MacioDavbus* s = nullptr;
        u32 dma_output(const u8*, u32, bool) override { return 0; }
        u32 dma_input(u8* data, u32 len) override;
    };

    // consume: a read by the CPU (Burgundy's "extend" moves on).
    u32 read_reg(u32 reg, bool consume);
    void write_reg(u32 reg, u32 value);
    void start_ticks();
    void tick(u64 now);
    static void tick_cb(void* c, u64 now) { static_cast<MacioDavbus*>(c)->tick(now); }

    Scheduler& sched_;
    AudioSink* sink_ = nullptr;
    DbdmaChannel* out_ch_ = nullptr;
    DbdmaChannel* in_ch_ = nullptr;
    Out out_;
    In in_;
    u32 control_ = 0, codec_ctrl_ = 0, byteswap_ = 0;
    u32 codec_[8] = {};
    Codec codec_type_;
    u8 breg_[256][4] = {};
    u8 bdata_ = 0;     // the byte a Burgundy read returned
    u8 bextend_ = 0;   // 1: the next status read shows "extend"
    u8 fifo_[kFifoBytes] = {};
    u32 fifo_head_ = 0, fifo_count_ = 0;  // bytes
    u32 in_avail_ = 0;                   // bytes of input (silence) ready
    bool ticking_ = false;
    u64 frames_ = 0;
    // The frame clock: fc_frames_ at fc_ps_, counting at rate() since.
    u64 fc_frames_ = 0, fc_ps_ = 0;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_DAVBUS_H
