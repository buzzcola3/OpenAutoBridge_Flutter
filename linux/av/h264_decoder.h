// H.264 decode pipeline with zero-copy DropBuffer exchange.
//
// Architecture:
//   GStreamer thread (H264Decoder)  →  DropBuffer  →  Flutter GL thread (renderer)
//
// The decoder runs as fast as frames arrive.  Flutter displays at vsync cadence.
// Frames are dropped if Flutter cannot keep up — never the other way around.
//
// Frame formats:
//   - DMA-BUF NV12 (zero-copy: EGL import of decoder memory)
//   - CPU NV12      (Y+UV plane upload → NV12→RGB shader)
//   - CPU RGBA      (software fallback: direct texture upload)

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// FrameFormat — what the decoder produced
// ---------------------------------------------------------------------------
enum class FrameFormat {
    kNone,
    kRGBA,       ///< CPU RGBA pixels in `pixels` vector
    kNV12_CPU,   ///< CPU-mapped NV12: Y plane + interleaved UV plane
    kNV12_DMABUF ///< DMA-BUF fd(s) for NV12 planes (zero-copy)
};

// ---------------------------------------------------------------------------
// FrameSlot — one physical frame buffer
// ---------------------------------------------------------------------------
// Exists in two instances inside DropBuffer.
// May carry RGBA pixels, CPU NV12 planes, or DMA-BUF fd(s).

struct FrameSlot {
    FrameFormat format = FrameFormat::kNone;

    // ── CPU RGBA path ──
    std::vector<uint8_t> pixels;          ///< RGBA pixel data (format==kRGBA)

    // ── CPU NV12 path ──
    std::vector<uint8_t> y_plane;         ///< Y  plane (w × h, GL_R8)
    std::vector<uint8_t> uv_plane;        ///< UV plane ((w/2)×(h/2)×2, GL_RG8)
    int  y_stride  = 0;                   ///< Y  bytes per row
    int  uv_stride = 0;                   ///< UV bytes per row

    // ── DMA-BUF zero-copy path (format==kNV12_DMABUF) ──
    int      y_dma_fd     = -1;           ///< dup'd DMA-BUF fd for Y (or whole NV12)
    int      uv_dma_fd    = -1;           ///< dup'd DMA-BUF fd for UV (-1 = same as y)
    uint32_t y_dma_offset = 0;            ///< byte offset to Y plane in fd
    uint32_t uv_dma_offset = 0;           ///< byte offset to UV plane in fd
    uint32_t y_dma_stride = 0;            ///< Y plane pitch in DMA-BUF
    uint32_t uv_dma_stride = 0;           ///< UV plane pitch in DMA-BUF

    // ── Common ──
    int  width     = 0;
    int  height    = 0;
    int  stride    = 0;                   ///< bytes per row (RGBA path only)
    bool dirty     = false;               ///< new data since last GL consume

    /// Close any owned DMA-BUF fds
    void close_dma_fds();
};

// ---------------------------------------------------------------------------
// DropBuffer — double-buffered frame exchange
// ---------------------------------------------------------------------------
// Shared state between GStreamer thread and Flutter GL thread.
//
// Slot ownership rules:
//   displaying_slot + in_use==true   → Flutter GL thread owns, decoder must not write
//   displaying_slot + in_use==false  → nobody owns, safe for decoder
//   pending_slot                     → decoder owns (will be overwritten)
//   neither                          → free, safe for decoder

struct DropBuffer {
    FrameSlot slots[2];                             ///< double buffer

    std::atomic<FrameSlot*> pending_slot{nullptr};  ///< GStreamer writes (atomic)
    FrameSlot* displaying_slot{nullptr};            ///< Flutter reads
    bool       in_use{false};                       ///< true while Flutter holds displaying
    std::mutex mutex;                               ///< protects displaying_slot + in_use

    /// Set by GL thread when DMA-BUF EGL import fails at runtime.
    /// Decoder reads this and switches to NV12-CPU extraction.
    std::atomic<bool> dmabuf_import_failed{false};
};

// ---------------------------------------------------------------------------
// H264Decoder — asynchronous GStreamer H.264 decoder
// ---------------------------------------------------------------------------
// Owns the GStreamer pipeline.  Runs on GStreamer's streaming thread.
// Never touches EGL or GL — all GPU work is done by the renderer.
//
// Output format priority:
//   1. NV12 DMA-BUF  (zero-copy, HW decoder → EGL import)
//   2. NV12 CPU      (HW or SW decoder → shader conversion on GPU)
//   3. RGBA CPU      (decodebin + videoconvert, pure software fallback)

class H264Decoder {
public:
    using FrameReadyCallback = std::function<void()>;

    H264Decoder();
    ~H264Decoder();

    /// Wire the shared DropBuffer.  Must be called before start().
    void set_buffer(DropBuffer* buf);

    /// Callback invoked (from GStreamer thread) when a new decoded frame is
    /// available in the DropBuffer.  Typically used to call
    /// fl_texture_registrar_mark_texture_frame_available().
    void set_frame_ready_callback(FrameReadyCallback cb);

    /// Build and start the GStreamer pipeline.
    void start();

    /// Stop the pipeline.  Safe to call multiple times.
    void stop();

    /// Push raw H.264 byte-stream data into the pipeline.
    /// Non-blocking; data is copied internally.  Thread-safe.
    void push_h264(const uint8_t* data, size_t size);

private:
    struct Impl;
    Impl* impl_;
};
