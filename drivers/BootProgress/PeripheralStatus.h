// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PAGANI_PERIPHERAL_STATUS_H
#define PAGANI_PERIPHERAL_STATUS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PAGANI_KEYS, PAGANI_TOUCH, PAGANI_PANEL, PAGANI_SPI, PAGANI_SPI_FIFO, PAGANI_PERIPHERALS };
// Display path hand-off (Stage7Probe): stop the diagnostic drawing before the first system frame (0: not now), give the screen back
// after a failed first frame, and record a display stage with a value.
int boot_progress_display_handoff(void);
void boot_progress_display_failed(void);
void boot_progress_display_status(const char *detail, uint64_t value);
void boot_progress_peripheral_enable(int enabled);
void boot_progress_platform_enable(int enabled);
enum { PAGANI_LEGACY_GPU_ROWS = 7, PAGANI_STATUS_BYTES = 80 };
int pagani_gpu_status(char lines[PAGANI_LEGACY_GPU_ROWS][PAGANI_STATUS_BYTES]);
void boot_progress_peripheral(unsigned device, const char *stage, int32_t result, uint32_t value, uint64_t reports);
void boot_progress_touch_spi(uint32_t firmware, uint32_t disabled, uint32_t state, uint32_t irq, uint32_t tx, uint32_t rx);
void boot_progress_touch_dma(uint32_t tx, uint32_t rx, uint32_t txin, uint32_t rxin);
void boot_progress_touch_read(uint32_t length, uint32_t head, uint32_t pins, uint32_t dma,
                              uint32_t state, uint32_t txin, uint32_t rxin, uint32_t delay);
void boot_progress_touch_power(uint32_t address, uint32_t hardware, uint32_t tcs, uint32_t state,
                               uint32_t cmd0, uint32_t cmd1, uint32_t cmd2);
void boot_progress_touch_hid(uint32_t binding, uint32_t usage, uint32_t events, uint32_t digitizers,
                             uint32_t edges, uint32_t input, uint64_t output, uint32_t mask);
void boot_progress_touch_smmu(uint32_t slot, uint32_t bank, uint32_t smr, uint32_t s2cr);
void boot_progress_peripheral_frame(void);
// GPU status rows of the A830 driver (Adreno830 adreno830_status_publish): up to four rows of at most 66 characters.
// Stored only; the 1 Hz observer draws them on text rows 10-13 when they changed, also after the display hand-off.
enum { PAGANI_GPU_ROWS = 4, PAGANI_GPU_ROW_BYTES = 68, PAGANI_GPU_TOP_ROW = 10 };
void boot_progress_gpu_rows(const char (*rows)[PAGANI_GPU_ROW_BYTES], unsigned count);
// Display probe rows (NativeMobileDisplay DpuProbe, ARCHITECTURE 5.3.3 and M12): up to five rows on text rows 14-18, stored and drawn
// exactly like the GPU rows (only when they changed, by the 1 Hz observer). PAGANI_STATUS_LINE is the height of one text row in pixels.
enum { PAGANI_DPU_ROWS = 5, PAGANI_DPU_TOP_ROW = 14, PAGANI_STATUS_LINE = 42 };
void boot_progress_dpu_rows(const char (*rows)[PAGANI_GPU_ROW_BYTES], unsigned count);
// The CPU frequency row of PaganiArmCpu (pagani-cpufreq, its CpuFreqGovernor.h), e.g. "CPU 3532/4320MHz lv15/15 ld 34/12% boost 3 thr 0
// err 0" or "CPU scmi off: timeout lvl get": at most PAGANI_CPU_ROW_WIDTH characters (longer text is cut), stored and drawn like the GPU
// rows (by the 1 Hz observer, only when the text changed, also after the display hand-off) on text row PAGANI_CPU_ROW, or on
// PAGANI_CPU_ROW_LOW right below the display probe rows while those are shown (pagani-dpu-probe). NULL or "" removes the row. Thread context.
enum { PAGANI_CPU_ROW = PAGANI_DPU_TOP_ROW, PAGANI_CPU_ROW_LOW = PAGANI_DPU_TOP_ROW + PAGANI_DPU_ROWS, PAGANI_CPU_ROW_WIDTH = 65 };
void boot_progress_cpu_row(const char *text);
// The framebuffer rows [y0, y1) the status rows (the SLOW row 8 and the FPS row 9, GPU rows 10-13, display probe rows 14-18, the CPU row 14
// (19 with the display probe rows) and the blank rows between them) occupy; 0 when nothing is shown. The presentation paths leave this band
// alone so the rows stay visible without being redrawn each frame (four loads, no lock).
int boot_progress_gpu_band(unsigned *y0, unsigned *y1);
// The number of frames the display path has presented so far (one store, no lock, every frame); the panic screen prints it, so a photo
// of a panic tells whether the screen was still being refreshed.
void boot_progress_display_frames(uint64_t frames);
// The frame rate (BootProgress FrameRate.h): every frame that reached the panel, with its path and the mach time it got there (0: now).
// PAGANI_FRAME_CPU: a CPU copy (or blank fill) into the scanout finished; PAGANI_FRAME_GPU: the GPU presenter's copy finished, or with
// page flips the flip latched. Two atomic adds and one store, no lock, any thread. With pagani-fps=1 the 1 Hz observer shows
// "FPS 58 (31-60) | ft 17.2 p99 41.5 max 88.0ms | gpu 45 cpu 13" on text row PAGANI_FPS_ROW (part of the band above): frames in the last
// second (range over the last ten seconds), the last second's frame times (average, 99th percentile, largest) and its frames per path;
// with the ramoops log on it also logs that line once a second while frames come (kind 'F') and each frame interval above 50 ms as a
// jitter record (kind 'J', rate-limited). pagani-fps=2: the log only. Default 1 with the GPU status rows (pagani-gpu=1, pagani-gpu-status
// not 0) or pagani-platform-status=1, else 0.
enum { PAGANI_FRAME_CPU = 0, PAGANI_FRAME_GPU = 1, PAGANI_FPS_ROW = 9 };
void boot_progress_frame(unsigned path, uint64_t when);
// Where a frame's time went (pagani-fps, the same ring): the display path's stages of one frame, mach ticks, 0 where a stage did not
// happen. With boot_progress_frame_timed instead of boot_progress_frame the 1 Hz observer also shows, on text row PAGANI_SLOW_ROW right
// above the FPS row (part of the band), the slowest frame interval of the last second with its breakdown,
// "SLOW 88.0ms gap 70.1 off 0.2 take 0.3 copy 17.0 done 0.1 fb0 to0" (gap: from the previous frame's arrival, off: arrival to offer,
// take: offer to taken, copy: taken to copied, done: copied to presented, " done" only while the row fits 65 characters; fb: CPU
// fallbacks this second, refused offers and failed GPU copies; to: the GPU client's empty waits this second), or, without a frame
// interval, "SLOW -" with the stage the display path waits in and for how long ("SLOW - copy 2.3s fb0 to0"; "idle": no new frame
// reached the display path). The J records carry the same breakdown for the late frame, and the F line the SLOW row while it matters.
struct pagani_frame_timing {
    uint64_t arrival;   // the compositor's swap became the display's pending head (NativeLegacyDisplay kickOnce, its first time)
    uint64_t gap;       // ticks since the previous frame's arrival (0: none before it)
    uint64_t offer;     // offered to the GPU client, or the CPU copy queued (synchronous copy: started)
    uint64_t taken;     // the GPU client took it (NativeGOPPresenter waitSwap returned it), or the CPU copy started
    uint64_t copied;    // the client's completeSwap, or the CPU copy ended
    uint32_t flags;     // PAGANI_TIMING_*
    uint32_t reserved;
};
enum {
    PAGANI_TIMING_REFUSED = 1,      // the GPU offer was refused: copied by the CPU
    PAGANI_TIMING_FAILED = 2,       // the GPU copy failed: copied again by the CPU
    PAGANI_TIMING_BYPASS = 4,       // the GPU path was skipped after a failed GPU copy (this frame's, or the one before's when that one
                                    // had retired early)
    PAGANI_TIMING_DEFERRED = 8,     // waited for a free page-flip buffer
    PAGANI_TIMING_FLIP = 16,        // presented by a page flip (at its latch)
    PAGANI_TIMING_FLAGS = 31,
};
void boot_progress_frame_timed(unsigned path, uint64_t when, const struct pagani_frame_timing *timing);
// The stage the display path reached at `when` (mach ticks, 0: now), the frame on its way while there is one, else the head frame
// waiting for it (ARRIVED) or nothing (IDLE: presented or lost, the path waits for the compositor's next frame): one store, no lock,
// any thread. The SLOW row names the part of the way it is in: off (ARRIVED), take (OFFERED), copy (TAKEN), done (COPIED), idle.
enum {
    PAGANI_STAGE_NONE, PAGANI_STAGE_ARRIVED, PAGANI_STAGE_OFFERED, PAGANI_STAGE_TAKEN, PAGANI_STAGE_COPIED, PAGANI_STAGE_IDLE,
    PAGANI_STAGES
};
void boot_progress_frame_stage(unsigned stage, uint64_t when);
// Display path events, counted per second for the SLOW row and the log (one atomic add, any thread): a GPU offer refused (sheng-fb-gpu=1),
// a GPU copy failed (then the CPU copy, or with early retirement the frame lost), one of those after early retirement, an empty wait of
// the GPU client (NativeGOPPresenter waitSwap timed out), a software vsync tick that came after its deadline (the F record's vl, pagani-diag).
enum { PAGANI_DISPLAY_REFUSED, PAGANI_DISPLAY_FAILED, PAGANI_DISPLAY_EARLY_FAILED, PAGANI_DISPLAY_TIMEOUT, PAGANI_DISPLAY_VSYNC_LATE,
       PAGANI_DISPLAY_EVENTS };
void boot_progress_display_event(unsigned event);
enum { PAGANI_SLOW_ROW = PAGANI_FPS_ROW - 1 };
// The persistent log of the photo-free test loop (RamoopsLog.c, pagani-ramoops=1): one line into the Android ramoops region, read
// back from Android after the next reset (the gpu-test-loop host tools). The line is in DRAM when the call returns, so trace a risky
// access before making it. Interrupt-safe; a cheap no-op while the log is off (boot_progress_trace_enabled() == 0).
void boot_progress_trace(const char *text);
void boot_progress_trace_values(const char *label, uint64_t a, uint64_t b);
int boot_progress_trace_enabled(void);
#ifdef __cplusplus
}
#endif
#endif
