// capture: the device abstraction. main.cc and the run loop use only this.
// soapy.cc is the SoapySDR implementation, synth.cc a synthetic source used by
// the tests and for smoke-checks.
#pragma once
#include <complex>
#include <string>
#include <vector>

struct Capture {
    virtual ~Capture() {}
    virtual double rate() const = 0;     // SDR sample rate (Hz), integer multiple of 12000
    virtual double center() const = 0;   // LO / center frequency (Hz)
    // Fill up to maxn IQ samples. Return count (0 = none yet, <0 = end/error).
    // Sets t_recv = CLOCK_REALTIME (seconds) of the NEWEST returned sample.
    virtual int read(std::complex<float> *buf, int maxn, double &t_recv) = 0;
};

// How the LO (center) is placed when not given an explicit frequency.
//   Auto: centered in the channel cluster, clear of the DC spike (lowest rate).
//   Edge: just below all channels, every channel at a positive offset (one-sided).
//   Fixed: use want_center verbatim.
enum class CenterMode { Auto, Edge, Fixed };

struct CaptureConfig {
    std::string driver;            // Soapy device args, or "synth"
    double gain = -1;              // dB, <0 = device default / AGC
    double ppm = 0;
    std::string antenna;
    std::vector<double> dials;     // channel dial frequencies (Hz)
    double guard = 10000;          // Hz a channel must stay clear of the LO and each band edge
    double want_rate = 0;          // 0 = auto
    CenterMode center_mode = CenterMode::Auto;
    double want_center = 0;        // LO frequency, used only when center_mode == Fixed
};

// auto-resolution (util.cc)
double resolve_center(const CaptureConfig &c);
// smallest integer-x12k rate >= the needed span that lies in `ranges`
// (flat [min,max,min,max,...], empty = any rate). 0 on failure.
double resolve_rate(const CaptureConfig &c, double center, const std::vector<double> &ranges);
// largest |dial - center| over the channels (the channel that drives the rate)
double max_offset(const CaptureConfig &c, double center);
// Validate the channels against the chosen center/rate. Returns a non-empty error
// string if any channel's passband crosses +/-rate/2 (it would alias). Emits a
// stderr warning for a channel within `guard` of a band edge or of the LO.
std::string check_channels(const CaptureConfig &c, double center, double rate);
// true iff rate is a positive integer multiple of 12000 (within 1 Hz). The whole
// pipeline assumes exact integer decimation fs->12000. A non-multiple would skew
// the audio rate while still being stamped 12000, so callers must reject it.
bool rate_ok(double rate);

// Build a SoapySDR device-args string from a plain device name: "hackrf" ->
// "driver=hackrf" (empty -> empty, i.e. first device). -driver is a device name,
// not Soapy args. The caller rejects any value containing '='.
std::string normalize_driver(const std::string &name);

// device factories (synth-vs-soapy choice is inlined in main.cc)
Capture *make_synth(const CaptureConfig &c, std::string &err);    // synth.cc
Capture *make_soapy(const CaptureConfig &c, std::string &err);    // soapy.cc
