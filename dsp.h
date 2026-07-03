// dsp: one per-channel signal chain. NCO mix, integer-decimating FIR, then either
// an SSB demod (USB/LSB) to real audio or the raw complex baseband (IQ), emitted as
// int16 or float32 bytes.
//
//   wideband IQ -> mix(-delta_f) -> decimate(fs -> out_rate) -> SSB demod or IQ -> bytes
//
// A wrong sideband or a delta_f sign error here fails silently (plausible-looking
// output, not a crash), so sideband selection and audio frequency are covered by
// behavioral tests in test_dsp.cc.
#pragma once
#include "capture.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

class ChannelChain {
public:
    // delta_hz : channel dial minus SDR center (positive: channel above the LO)
    // fs       : SDR sample rate. MUST be an integer multiple of out_rate
    // out_rate : channel output sample rate (Hz)
    // mode     : USB/LSB (real audio) or IQ (complex baseband, no demod)
    // format   : I16 (scaled int16) or F32 (raw float32)
    ChannelChain(double delta_hz, int fs, int out_rate, Mode mode, Format format);

    // Push n wideband IQ samples. Append output samples to `out` as little-endian
    // bytes in the channel's mode/format. Filter state carries (streaming-safe).
    void process(const std::complex<float>* iq, size_t n, std::vector<uint8_t>& out);

    int out_rate() const { return out_rate_; }

private:
    void put(std::vector<uint8_t>& out, float v) const;   // one value, in `format_`

    Mode mode_;
    Format format_;
    float demod_sign_;   // USB = -1, LSB = +1 on the Hilbert term (unused for IQ)

    // NCO (digital down-conversion): a recursive complex phasor (osc_ *= rot_ per
    // sample) instead of a per-sample sin/cos. rot_ holds the exact double rotation
    // e^(-j2pi df/fs), so the frequency is exact (no audio offset vs an ideal LO);
    // osc_ is renormalized periodically to hold unit magnitude against float drift.
    std::complex<double> rot_, osc_;
    int osc_count_;

    // decimating low-pass FIR (complex), integer factor D = fs / out_rate
    int out_rate_;
    int D_, dec_count_;
    std::vector<float> lp_;
    std::vector<std::complex<float>> lp_hist_;
    size_t lp_pos_;

    // USB demod: Hilbert FIR on Q, matched delay on I  (usb = I_delayed - H{Q})
    std::vector<float> hil_;
    int hdelay_;
    std::vector<float> i_hist_, q_hist_;
    size_t h_pos_;
};
