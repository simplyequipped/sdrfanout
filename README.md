# sdrfanout
Fan one SDR out to several offset narrowband channels/streams:
USB or LSB audio, or raw complex IQ. Built for feeding multiple decoders (ft8mon,
wsprmon, etc) from a single radio, but the stream format is open so any consumer can
read it.

## Build

Requires SoapySDR plus the device modules for your SDR:

```
  sudo apt install libsoapysdr-dev soapysdr-module-rtlsdr \
    soapysdr-module-hackrf soapysdr-module-airspy \
    soapysdr-module-airspyhf
  make
```

Unit tests:

```
  make test
```

## Usage

```
usage: sdrfanout -ch <dial_hz>:<path> [-ch ...] [options]
  -driver <dev>           SoapySDR device name (ex. hackrf, rtlsdr, etc) or 'synth'
  -gain <db|auto>         default: auto (AGC / device default)
  -rate <hz|auto>         sample rate in Hz, default: auto
                          auto: smallest multiple of the output rate spanning all channels
  -center <hz|auto|edge>  center frequency in Hz, default: auto
                          auto: centered amidst channels, offset from DC by *guard*
                          edge: *guard* Hz below the lowest channel
  -guard <hz>             offset around center (DC) and band edges in Hz, default: 10000
  -ppm <n>                frequency correction, default: 0
  -antenna <name>         Soapy antenna, default: not set (device default)
  -buffer <sec>           per-channel output buffer in seconds, default: 1
  -threads <n>            DSP worker threads, default: auto (one per channel, capped at cores)
  -outrate <hz>           channel output sample rate in Hz, default: 12000
  -mode <usb|lsb|iq>      default channel output mode: usb (iq = raw complex baseband)
  -format <i16|f32>       output sample type, default: i16
  -ch <dial>[:<path>[:<mode>[:<outrate>]]]
                          channel dial freq (Hz) + output FIFO, with optional per-
                          channel mode and outrate (empty field = use the global)
```

Each `-ch` defines one channel: a dial frequency and a FIFO output path. The output path
is optional. With just a dial, sdrfanout creates the FIFO at `/tmp/sdrfanout/<dial>.fifo`
(deterministic, so a consumer can find it without coordination) and prints the resolved
path to stderr. Give an explicit `:<path>` to place it elsewhere, or `:-` to write the
stream to stdout. The trailing `:<mode>` and `:<outrate>` fields override the global
`-mode` and `-outrate` for that channel; leave a field empty to keep the global (for
example `-ch 7074000::iq` sets IQ output with the default outrate). Two co-located
decoders off one radio:

```
sdrfanout -driver hackrf -ch 7038600 -ch 7074000
ft8mon  -card stream /tmp/sdrfanout/7074000.fifo
wsprmon -card stream /tmp/sdrfanout/7038600.fifo
```

`-mode` picks what each channel emits. `usb` and `lsb` demodulate to real
single-sideband audio. `iq` skips the demod and streams the complex baseband. `-format` picks the sample element type, `i16` (scaled int16) or
`f32` (raw unscaled float32, approx. -1 to 1), and applies to all channels. For the audio modes
keep `-outrate` at 48k or lower, opposite-sideband rejection weakens at higher output
rates.

The SDR sample rate is chosen to be an integer multiple of the channel output rate, so each
channel decimates by a whole number. `-rate auto` picks the smallest multiple that
spans the channels, which can be up to one output rate above the minimum they need.
That rounding is coarse at high output rates. When channels use different output
rates, the SDR sample rate must be a multiple of every one, so it is sized to their least
common multiple.

sdrfanout sets the SDR analog bandwidth to the narrowest the device offers that
still covers every channel. If the device's narrowest filter is wider than the
sample rate the channels need, out-of-band signals can alias onto the channels.
sdrfanout warns when that happens, raise `-rate` for a clean window if the extra
sample rate is affordable. A device that reports no bandwidth control keeps its
default.

## Stream Format

All modes are little-endian. The audio modes (`usb`, `lsb`) frame their output; `iq`
mode does not.

In an audio mode each channel is a continuous sequence of `[sdrf_hdr][nsamples x
sample]`. The sample is `int16` by default, or `float32` when the header's
`SDRF_FLAG_F32` bit is set. `rate_hz` is the channel's output rate and `t_usec` is
the UTC capture time of the frame's first sample.

```c
struct sdrf_hdr {            // 24 bytes, packed
    uint32_t magic;         // 0x46524453  ("SDRF"), fixed resync anchor
    uint16_t version;       // 1
    uint16_t flags;         // bit0 = disk/replay, bit1 = float32 samples (else int16)
    uint32_t rate_hz;       // channel output rate
    uint32_t nsamples;      // samples following this header (int16 or float32 per flags)
    uint64_t t_usec;        // UTC epoch microseconds of the first sample
};
```

To (re)synchronise, scan the byte stream for `magic`, then sanity-check the
header (`version==1`, `rate_hz` and `nsamples` reasonable) before trusting it.

In `iq` mode there is no header: the channel is a raw stream of interleaved I,Q
samples (`int16` or `float32` per `-format`) at `-outrate`.

Minimal consumer (audio):

```c
#include <stdio.h>
#include <stdint.h>
struct sdrf_hdr { uint32_t magic, version_flags; uint32_t rate_hz, nsamples; uint64_t t_usec; };
// (read header, validate magic, then read nsamples int16, repeat)
int main(void) {
    struct sdrf_hdr h;
    while (fread(&h, sizeof h, 1, stdin) == 1) {
        if (h.magic != 0x46524453) { /* resync: scan for magic */ continue; }
        int16_t pcm[4096];
        for (uint32_t left = h.nsamples; left; ) {
            uint32_t take = left < 4096 ? left : 4096;
            if (fread(pcm, sizeof(int16_t), take, stdin) != take) return 0;
            /* ... use pcm[] (int16 audio at h.rate_hz). h.t_usec = time of pcm[0] ... */
            left -= take;
        }
    }
    return 0;
}
```

## Acknowledgements

- USB demod approach follows the phasing method used in [ft8mon](https://github.com/rtmrtmrtmrtm/ft8mon)
- Device abstraction via [SoapySDR](https://github.com/pothosware/SoapySDR)
- Claude AI contributed to this project
