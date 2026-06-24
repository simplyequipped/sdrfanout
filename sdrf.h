// sdrf: "sdrfanout stream frame", the on-wire format sdrfanout writes to each channel FIFO.
// A continuous sequence of [sdrf_hdr][nsamples * int16 LE PCM], little-endian.
// Deliberately simple and open so any consumer can read it (see README "Stream format").
#pragma once
#include <cstdint>

static const uint32_t SDRF_MAGIC   = 0x46524453;  // "SDRF" on the wire (LE bytes 53 44 52 46)
static const uint16_t SDRF_VERSION = 1;
static const uint16_t SDRF_FLAG_DISK = 0x0001;    // bit0 = ndiskdat (disk/replay source)

#pragma pack(push, 1)
struct sdrf_hdr {
    uint32_t magic;     // SDRF_MAGIC
    uint16_t version;   // SDRF_VERSION
    uint16_t flags;     // SDRF_FLAG_* (0 for live capture)
    uint32_t rate_hz;   // 12000
    uint32_t nsamples;  // count of int16 samples that follow this header
    uint64_t t_usec;    // UTC epoch microseconds of the FIRST sample in this frame
};
#pragma pack(pop)

static_assert(sizeof(sdrf_hdr) == 24, "sdrf_hdr must be 24 bytes");
