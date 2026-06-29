// soapy: the SoapySDR implementation of Capture. The ONLY file that touches
// SoapySDR (so the rest of sdrfanout, and the tests, stay device-agnostic).
#include "capture.h"
#include "util.h"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Logger.h>
#include <SoapySDR/Types.hpp>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

class SoapyCapture : public Capture {
public:
    SoapyCapture(SoapySDR::Device *d, SoapySDR::Stream *s, double rate, double center)
        : dev_(d), st_(s), rate_(rate), center_(center) {}
    ~SoapyCapture() override {
        if (dev_ && st_) { dev_->deactivateStream(st_); dev_->closeStream(st_); }
        if (dev_) SoapySDR::Device::unmake(dev_);
    }
    double rate() const override { return rate_; }
    double center() const override { return center_; }

    int read(std::complex<float> *buf, int maxn, double &t_recv) override {
        void *buffs[1] = {buf};
        int flags = 0;
        long long timeNs = 0;
        int r = dev_->readStream(st_, buffs, (size_t)maxn, flags, timeNs, 200000);  // 200 ms
        t_recv = now();
        if (r >= 0) return r;
        if (r == SOAPY_SDR_TIMEOUT) return 0;
        if (r == SOAPY_SDR_OVERFLOW) {                 // driver dropped samples, keep running
            std::fprintf(stderr, "sdrfanout: SDR overflow\n");
            return 0;
        }
        std::fprintf(stderr, "sdrfanout: readStream error: %s\n", SoapySDR::errToStr(r));
        return -1;
    }

private:
    SoapySDR::Device *dev_;
    SoapySDR::Stream *st_;
    double rate_, center_;
};

Capture *make_soapy(const CaptureConfig &c, std::string &err) {
    // Quiet SoapySDR's own [INFO] chatter (e.g. the driver's "Opening ..."), keeping
    // warnings and errors. sdrfanout prints its own device summary line.
    SoapySDR::setLogLevel(SOAPY_SDR_WARNING);
    SoapySDR::Device *dev = nullptr;
    std::string args = normalize_driver(c.driver);    // "hackrf" -> "driver=hackrf"
    try {
        dev = SoapySDR::Device::make(args);           // "" = first enumerated device
    } catch (const std::exception &e) {
        err = std::string("SoapySDR::make: ") + e.what();
        return nullptr;
    }
    if (!dev) { err = "no SDR device found (driver=\"" + c.driver + "\")"; return nullptr; }

    std::vector<double> ranges;
    for (auto &rg : dev->getSampleRateRange(SOAPY_SDR_RX, 0)) {
        ranges.push_back(rg.minimum());
        ranges.push_back(rg.maximum());
    }

    // The analog bandwidth the channels need, chosen from what the device reports.
    // listBandwidths is the discrete set (e.g. an RSP's IF filters); getBandwidthRange
    // is a continuous span (e.g. a HackRF). Either may be empty on drivers that do not
    // expose bandwidth control, in which case resolve_bandwidth returns 0 and the
    // device keeps its own default.
    std::vector<double> bw_discrete = dev->listBandwidths(SOAPY_SDR_RX, 0);
    std::vector<double> bw_ranges;
    for (auto &rg : dev->getBandwidthRange(SOAPY_SDR_RX, 0)) {
        bw_ranges.push_back(rg.minimum());
        bw_ranges.push_back(rg.maximum());
    }

    double center = resolve_center(c);
    double bandwidth = resolve_bandwidth(min_span(c, center), bw_discrete, bw_ranges);
    double rate = resolve_rate(c, center, ranges);   // rate covers the channels, see the bw warning
    if (rate <= 0) {
        err = "device supports no integer-multiple-of-12kHz rate that spans the channels";
        SoapySDR::Device::unmake(dev);
        return nullptr;
    }
    if (!rate_ok(rate)) {                                  // e.g. a bad -rate override
        err = "rate " + std::to_string((long)rate) + " is not an integer multiple of 12000";
        SoapySDR::Device::unmake(dev);
        return nullptr;
    }

    try {
        dev->setSampleRate(SOAPY_SDR_RX, 0, rate);
        rate = dev->getSampleRate(SOAPY_SDR_RX, 0);       // device may round, use what it'll deliver
        if (bandwidth > 0) {
            dev->setBandwidth(SOAPY_SDR_RX, 0, bandwidth);   // open the analog filter to cover the channels
            bandwidth = dev->getBandwidth(SOAPY_SDR_RX, 0);  // what the device actually applied
            if (bandwidth > rate)                            // narrowest filter wider than Nyquist
                std::fprintf(stderr, "sdrfanout: warning: analog bandwidth %.0f Hz exceeds the "
                             "sample rate %.0f Hz, out-of-band signals will alias onto the channels "
                             "(raise -rate for a clean window)\n", bandwidth, rate);
        }
        dev->setFrequency(SOAPY_SDR_RX, 0, center);
        if (c.ppm != 0) {
            if (dev->hasFrequencyCorrection(SOAPY_SDR_RX, 0))
                dev->setFrequencyCorrection(SOAPY_SDR_RX, 0, c.ppm);
            else                                          // don't let -ppm be a silent no-op
                std::fprintf(stderr, "sdrfanout: warning: -ppm %g ignored "
                             "(this device has no frequency correction)\n", c.ppm);
        }
        if (!c.antenna.empty()) dev->setAntenna(SOAPY_SDR_RX, 0, c.antenna);
        if (c.gain < 0) {
            if (dev->hasGainMode(SOAPY_SDR_RX, 0)) dev->setGainMode(SOAPY_SDR_RX, 0, true);  // AGC
        } else {
            dev->setGain(SOAPY_SDR_RX, 0, c.gain);
        }
    } catch (const std::exception &e) {
        err = std::string("SoapySDR config: ") + e.what();
        SoapySDR::Device::unmake(dev);
        return nullptr;
    }
    if (!rate_ok(rate)) {                                 // device rounded to a non-12k-multiple
        err = "device delivered rate " + std::to_string((long)rate) +
              ", not an integer multiple of 12000";
        SoapySDR::Device::unmake(dev);
        return nullptr;
    }

    // The delivered rate may differ from what we sized for, so check the channels
    // against the actual center/rate: fail on a channel that would alias, warn on
    // one too near a band edge or the LO.
    std::string cerr = check_channels(c, center, rate);
    if (!cerr.empty()) {
        err = cerr;
        SoapySDR::Device::unmake(dev);
        return nullptr;
    }

    SoapySDR::Stream *st = nullptr;
    try {
        st = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);   // CF32 = interleaved complex<float>
        dev->activateStream(st);
    } catch (const std::exception &e) {
        err = std::string("SoapySDR stream: ") + e.what();
        SoapySDR::Device::unmake(dev);
        return nullptr;
    }

    char bwbuf[48] = "";
    if (bandwidth > 0) std::snprintf(bwbuf, sizeof bwbuf, "  bw=%.0f Hz", bandwidth);
    std::fprintf(stderr, "sdrfanout: %s  rate=%.0f Hz  center=%.0f Hz%s\n",
                 c.driver.empty() ? "(first device)" : c.driver.c_str(), rate, center, bwbuf);
    return new SoapyCapture(dev, st, rate, center);
}
