// dsp: one per-channel signal chain. NCO mix, integer-decimating FIR, USB demod.
//
//   wideband IQ  ──mix(−Δf)──►  ──decimate(Fs→12000)──►  ──USB demod──►  int16 audio
//
// This is the riskiest part of sdrfanout: a wrong sideband or a sign error on Δf
// produces plausible-looking garbage. test_dsp.cc validates it with synthetic IQ
// (checking sideband selection and audio frequency).
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

class ChannelChain {
public:
    static constexpr int OUT_RATE = 12000;

    // delta_hz : channel dial minus SDR center (positive: channel above the LO)
    // fs       : SDR sample rate. MUST be an integer multiple of OUT_RATE
    ChannelChain(double delta_hz, int fs);

    // Push n wideband IQ samples. Append decimated USB audio (int16) to `out`.
    // Filter state carries across calls (streaming-safe).
    void process(const std::complex<float>* iq, size_t n, std::vector<int16_t>& out);

private:
    // NCO (digital down-conversion): a recursive complex phasor (osc_ *= rot_ per
    // sample) instead of a per-sample sin/cos. rot_ holds the exact double rotation
    // e^(-j2pi df/fs), so the frequency is exact (no audio offset vs an ideal LO);
    // osc_ is renormalized periodically to hold unit magnitude against float drift.
    std::complex<double> rot_, osc_;
    int osc_count_;

    // decimating low-pass FIR (complex), integer factor D
    int D_, dec_count_;
    std::vector<float> lp_;
    std::vector<std::complex<float>> lp_hist_;
    size_t lp_pos_;

    // USB demod: Hilbert FIR on Q, matched delay on I  (usb = I_delayed − H{Q})
    std::vector<float> hil_;
    int hdelay_;
    std::vector<float> i_hist_, q_hist_;
    size_t h_pos_;
};
