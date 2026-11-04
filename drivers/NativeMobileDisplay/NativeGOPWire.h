// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual firmware scanout transfer, not a compositor or hardware vblank ABI.
#pragma once
#include <stdint.h>
enum { NGP_BGRX8888=1,NGP_RGBA16F_LINEAR=2,NGP_RGBA16F_ENCODED=3,NGP_VERSION=1 };
#define NGP_OVERLAY_API 1
enum { NGP_METHOD_OVERLAY=4 };
struct NGPOverlay {uint32_t first,rows;};
static_assert(sizeof(NGPOverlay)==8,"GOP overlay ABI");
// Half-open visible row ranges, excluding the diagnostic band. Invalid bands leave the full frame visible.
static inline void ngp_visible_rows(uint32_t height,NGPOverlay band,uint32_t ranges[4]) {
    ranges[0]=0;ranges[1]=height;ranges[2]=height;ranges[3]=height;
    if(band.first<=height && band.rows<=height-band.first && band.rows) {
        ranges[1]=band.first;ranges[2]=band.first+band.rows;
    }
}
struct NGPMode {uint32_t version,format;uint64_t width,height,stride,bytes;};
struct NGPPresent {uint64_t address,bytes;uint32_t width,height,stride,format;};
struct NGPTransfer {uint64_t sequence,bytes;};
// GPU hand-off (boot-arg sheng-fb-gpu=1): the display driver offers a finished frame instead of copying it. A privileged client waits for it
// (selector 2: the frame's pages are mapped read/write into the caller, at address), lets the GPU copy it into the scanout mapping (client
// memory type 1, writable) and then completes it (selector 3), which retires the swap. Nothing here copies pixels.
// NGPSwap.reserved is the display driver's id of the frame's mapping in the caller: the same id names the same mapping (same pages at the
// same address) for as long as it is handed out, so the caller may keep its GPU import of it; 0 means mapped for this frame only.
enum { NGP_METHOD_MODE=0,NGP_METHOD_PRESENT=1,NGP_METHOD_WAIT_SWAP=2,NGP_METHOD_COMPLETE_SWAP=3,NGP_MEMORY_SCANOUT_RW=1 };
struct NGPWait {uint64_t timeout_ms;};
struct NGPSwap {uint64_t sequence,address,bytes,stride;uint32_t width,height,format,reserved;};
struct NGPComplete {uint64_t sequence;uint32_t status,reserved;};
#define NGP_TIMING_API 1
// Optional completion suffix; legacy 16-byte completions retain the same meaning.
struct NGPCompleteTimed {NGPComplete done;uint32_t sync_us,encode_us,submit_us,wait_us,gpu_us,reserved;};
static_assert(sizeof(NGPCompleteTimed)==40,"GOP timed completion ABI");
static_assert(sizeof(NGPWait)==8 && sizeof(NGPSwap)==48 && sizeof(NGPComplete)==16,"GOP hand-off ABI");
static_assert(sizeof(NGPMode)==40 && sizeof(NGPPresent)==32 && sizeof(NGPTransfer)==16,"GOP transfer ABI");
// Additions within version 1. No record above changes size or meaning, so an older client and an older display driver keep working with
// each other: a method one side does not know answers kIOReturnUnsupported or kIOReturnBadArgument, and the caller falls back.
// - NGP_METHOD_BAND (4): no input, NGPBand out. The scanout rows [y0,y1) that the boot status overlay keeps for its text (rows text rows
//   of y1-y0 pixels together; y0==y1: none). A GPU client leaves them alone, like the CPU copy does.
// - NGP_METHOD_WAIT_SWAP2 (5): NGPWait in, NGPSwap2 out. WAIT_SWAP plus the byte offset, from the start of the scanout memory, of the
//   buffer this frame goes to (triple-buffered page flips, boot-arg pagani-fb-flip=1: 0, one frame or two frames; otherwise always 0).
//   The scanout memory then spans every buffer; NGPMode.bytes stays one frame. The status band of buffer 0 is copied along by the client.
enum { NGP_METHOD_BAND=4,NGP_METHOD_WAIT_SWAP2=5 };
struct NGPBand {uint32_t y0,y1,rows,reserved;};
struct NGPSwap2 {NGPSwap swap;uint64_t target_offset;};
static_assert(sizeof(NGPBand)==16 && sizeof(NGPSwap2)==56 && __builtin_offsetof(NGPSwap2,target_offset)==48,"GOP hand-off ABI additions");
