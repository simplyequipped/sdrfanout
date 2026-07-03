// sdrf: "sdrfanout stream frame", the on-wire format sdrfanout writes to a channel
// FIFO in an audio mode (USB/LSB). A continuous sequence of [sdrf_hdr][nsamples *
// sample], little-endian. The sample is int16 by default, or float32 when
// SDRF_FLAG_F32 is set. IQ mode writes raw headerless interleaved samples instead.
// Deliberately simple and open so any consumer can read it (see README "Stream format").
#pragma once
#include <cstdint>

static const uint32_t SDRF_MAGIC   = 0x46524453;  // "SDRF" on the wire (LE bytes 53 44 52 46)
static const uint16_t SDRF_VERSION = 1;
static const uint16_t SDRF_FLAG_DISK = 0x0001;    // bit0 = ndiskdat (disk/replay source)
static const uint16_t SDRF_FLAG_F32  = 0x0002;    // bit1 = samples are float32 (else int16)

#pragma pack(push, 1)
struct sdrf_hdr {
    uint32_t magic;     // SDRF_MAGIC
    uint16_t version;   // SDRF_VERSION
    uint16_t flags;     // SDRF_FLAG_* (0 for live int16 capture)
    uint32_t rate_hz;   // channel output sample rate
    uint32_t nsamples;  // count of samples that follow (int16 or float32 per flags)
    uint64_t t_usec;    // UTC epoch microseconds of the FIRST sample in this frame
};
#pragma pack(pop)

static_assert(sizeof(sdrf_hdr) == 24, "sdrf_hdr must be 24 bytes");
