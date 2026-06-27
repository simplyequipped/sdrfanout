# sdrfanout
Fan one SDR out to several offset narrowband channels, each as a stamped 12 kHz
sample rate audio stream. Built for feeding multiple decoders (ft8mon, wsprmon, etc)
from a single radio, but the stream format is open. Any consumer can read it.

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
                          auto: smallest multiple of 12k spanning all channels
  -center <hz|auto|edge>  center frequency in Hz, default: auto
                          auto: centered amidst channels, offset from DC by *guard*
                          edge: *guard* Hz below the lowest channel
  -guard <hz>             offset around center (DC) and band edges in Hz, default: 10000
  -ppm <n>                frequency correction, default: 0
  -antenna <name>         Soapy antenna, default: not set (device default)
  -buffer <sec>           per-channel output buffer in seconds, default: 1
  -threads <n>            DSP worker threads, default: auto (one per channel, capped at cores)
  -ch <dial>[:<path>]     channel dial freq (Hz) + output FIFO, path defaults to
                          /tmp/sdrfanout/<dial>.fifo, set path to '-' for stdout
```

Each `-ch` defines one channel: a dial frequency and a FIFO output path. The output path
is optional. With just a dial, sdrfanout creates the FIFO at `/tmp/sdrfanout/<dial>.fifo`
(deterministic, so a consumer can find it without coordination) and prints the resolved
path to stderr. Give an explicit `:<path>` to place it elsewhere, or `:-` to write the
stream to stdout. Two co-located decoders off one radio:

```
sdrfanout -driver hackrf -ch 7038600 -ch 7074000
ft8mon  -card stream /tmp/sdrfanout/7074000.fifo
wsprmon -card stream /tmp/sdrfanout/7038600.fifo
```

## Stream Format

Each channel is its own output: a continuous little-endian sequence of
`[sdrf_hdr][nsamples × int16 PCM]`. `t_usec` is the UTC capture time of
the frame's first sample.

```c
struct sdrf_hdr {            // 24 bytes, packed
    uint32_t magic;         // 0x46524453  ("SDRF"), fixed resync anchor
    uint16_t version;       // 1
    uint16_t flags;         // bit0 = ndiskdat (disk/replay), else 0
    uint32_t rate_hz;       // 12000
    uint32_t nsamples;      // int16 samples following this header
    uint64_t t_usec;        // UTC epoch microseconds of the first sample
};
```

To (re)synchronise, scan the byte stream for `magic`, then sanity-check the
header (`version==1`, `rate_hz==12000`, `nsamples` reasonable) before trusting it.

Minimal consumer:

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
            /* ... use pcm[] (12 kHz USB audio). h.t_usec = time of pcm[0] ... */
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
