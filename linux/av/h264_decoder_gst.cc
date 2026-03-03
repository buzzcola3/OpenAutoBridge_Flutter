// H.264 decode pipeline using GStreamer.
//
// Push raw H.264 byte-stream data in, get decoded frames out via appsink
// callback, deposited into a lock-free DropBuffer for the Flutter GL thread.
//
// Output format priority (tried per-sample at runtime):
//   1. NV12 DMA-BUF  — zero-copy EGL import of decoder DMA-BUF memory
//   2. NV12 CPU      — map NV12 planes, upload Y+UV textures
//   3. RGBA CPU      — videoconvert fallback
//
// Pipeline candidates (tried at init, first working wins):
//   1. vaapidecode  + vaapipostproc               (Intel/AMD desktop)
//   2. v4l2slh264dec                               (Allwinner Cedrus, RPi 5, etc.)
//   3. v4l2h264dec                                 (RK3588, i.MX, older RPi)
//   4. decodebin + videoconvert → RGBA             (pure software fallback)
//
// For HW decoders (#2, #3) the pipeline requests NV12 output with DMA-BUF
// caps preferred on the appsink.  A pad probe on the appsink sink pad
// intercepts ALLOCATION queries to provide a DMA-BUF allocator when the
// decoder supports it.

#include "h264_decoder.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <unistd.h>       // dup(), close()

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>
#include <gst/video/video-frame.h>
#include <gst/allocators/gstdmabuf.h>

/* ------------------------------------------------------------------ */
/*  DMA-BUF allocation probe (from cedrus_hw_h264)                    */
/* ------------------------------------------------------------------ */

/// Pad probe that intercepts ALLOCATION queries on the appsink sink pad.
/// When the decoder's caps indicate memory:DMABuf, we supply a DMA-BUF
/// allocator + VIDEO_META so the decoder exports with proper aligned strides.
static GstPadProbeReturn probe_allocation(GstPad* /*pad*/,
                                          GstPadProbeInfo* info,
                                          gpointer /*user_data*/) {
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM))
        return GST_PAD_PROBE_OK;

    GstQuery* q = gst_pad_probe_info_get_query(info);
    if (!q || GST_QUERY_TYPE(q) != GST_QUERY_ALLOCATION)
        return GST_PAD_PROBE_OK;

    GstCaps* alloc_caps = nullptr;
    gboolean need_pool = FALSE;
    gst_query_parse_allocation(q, &alloc_caps, &need_pool);

    bool is_dma = false;
    if (alloc_caps && gst_caps_get_size(alloc_caps) > 0) {
        const GstCapsFeatures* feat = gst_caps_get_features(alloc_caps, 0);
        if (feat && gst_caps_features_contains(feat, "memory:DMABuf"))
            is_dma = true;
    }

    if (is_dma) {
        GstAllocator* alloc = gst_dmabuf_allocator_new();
        if (alloc) {
            GstAllocationParams params;
            gst_allocation_params_init(&params);
            params.align = 63;  // 64-byte alignment mask
            gst_query_add_allocation_param(q, alloc, &params);
            gst_object_unref(alloc);
        }
        gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, nullptr);
        return GST_PAD_PROBE_HANDLED;
    }

    return GST_PAD_PROBE_OK;
}

/// Install the allocation probe on a pad.
static void install_allocation_probe(GstElement* elem, const char* pad_name) {
    GstPad* pad = gst_element_get_static_pad(elem, pad_name);
    if (!pad) return;
    gst_pad_add_probe(pad,
                      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM),
                      probe_allocation, nullptr, nullptr);
    gst_object_unref(pad);
}

/* ------------------------------------------------------------------ */
/*  H264Decoder::Impl                                                 */
/* ------------------------------------------------------------------ */

struct H264Decoder::Impl {
    GstElement* pipeline = nullptr;
    GstElement* appsrc   = nullptr;
    GstElement* appsink  = nullptr;
    bool        started  = false;
    bool        is_hw    = false;
    bool        is_nv12  = false;   // pipeline outputs NV12 (not RGBA)

    int push_count   = 0;
    int decode_count = 0;
    int fail_count   = 0;
    int dmabuf_count = 0;
    int nv12_cpu_count = 0;
    int rgba_count   = 0;

    DropBuffer*        buf = nullptr;
    FrameReadyCallback frame_ready_cb;

    /// Set by the GL thread (via DropBuffer flag) when DMA-BUF EGL import
    /// fails at runtime.  Causes decoder to switch to NV12-CPU extraction.
    bool dmabuf_runtime_disabled_cached = false;

    /* ── pipeline construction ── */

    struct PipelineCandidate {
        const char* label;
        bool        hw;
        bool        nv12;
        /// Build pipeline manually (needed for NV12 + DMA-BUF caps).
        GstElement* (*build_fn)(GstElement*& appsrc_out, GstElement*& appsink_out);
        /// Alternative: parse-launch string (used for RGBA candidates)
        const char* pipe_str;
    };

    /// Build a HW-decode pipeline that outputs NV12 with DMA-BUF preference.
    static GstElement* build_nv12_pipeline(const char* decoder_name,
                                           GstElement*& appsrc_out,
                                           GstElement*& appsink_out) {
        auto* src    = gst_element_factory_make("appsrc",    "src");
        auto* parse  = gst_element_factory_make("h264parse", "parse");
        auto* decode = gst_element_factory_make(decoder_name, "decode");
        auto* queue  = gst_element_factory_make("queue",     "queue");
        auto* sink   = gst_element_factory_make("appsink",   "sink");

        if (!src || !parse || !decode || !queue || !sink) {
            if (src)    gst_object_unref(src);
            if (parse)  gst_object_unref(parse);
            if (decode) gst_object_unref(decode);
            if (queue)  gst_object_unref(queue);
            if (sink)   gst_object_unref(sink);
            return nullptr;
        }

        // appsrc caps
        GstCaps* src_caps = gst_caps_new_simple(
            "video/x-h264",
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment",     G_TYPE_STRING, "au",
            nullptr);
        g_object_set(G_OBJECT(src),
                     "caps",        src_caps,
                     "format",      GST_FORMAT_TIME,
                     "stream-type", 0,
                     "is-live",     TRUE,
                     nullptr);
        gst_caps_unref(src_caps);

        // appsink caps: prefer DMA-BUF NV12, accept plain NV12 too
        GstCaps* sink_caps = gst_caps_new_empty();
        gst_caps_append(sink_caps,
            gst_caps_from_string("video/x-raw(memory:DMABuf),format=DMA_DRM"));
        gst_caps_append(sink_caps,
            gst_caps_new_simple("video/x-raw",
                                "format", G_TYPE_STRING, "NV12", nullptr));
        g_object_set(G_OBJECT(sink),
                     "caps",         sink_caps,
                     "emit-signals", TRUE,
                     "max-buffers",  1,
                     "drop",         TRUE,
                     "sync",         FALSE,
                     nullptr);
        gst_caps_unref(sink_caps);

        auto* pipe = gst_pipeline_new("h264-decode");
        gst_bin_add_many(GST_BIN(pipe), src, parse, decode, queue, sink, nullptr);
        if (!gst_element_link_many(src, parse, decode, queue, sink, nullptr)) {
            gst_object_unref(pipe);
            return nullptr;
        }

        // Install DMA-BUF allocation probe on appsink's sink pad
        install_allocation_probe(sink, "sink");

        appsrc_out  = src;
        appsink_out = sink;
        return pipe;
    }

    static GstElement* build_v4l2sl(GstElement*& src, GstElement*& sink) {
        return build_nv12_pipeline("v4l2slh264dec", src, sink);
    }

    static GstElement* build_v4l2(GstElement*& src, GstElement*& sink) {
        return build_nv12_pipeline("v4l2h264dec", src, sink);
    }

    bool try_candidate(const PipelineCandidate& c) {
        GstElement* p = nullptr;
        GstElement* as = nullptr;
        GstElement* ak = nullptr;

        if (c.build_fn) {
            p = c.build_fn(as, ak);
        } else if (c.pipe_str) {
            GError* err = nullptr;
            p = gst_parse_launch(c.pipe_str, &err);
            if (!p || err) {
                std::cout << "[H264Decoder] " << c.label
                          << " — parse failed: "
                          << (err ? err->message : "unknown") << "\n";
                if (err) g_error_free(err);
                if (p) gst_object_unref(p);
                return false;
            }
        }

        if (!p) {
            std::cout << "[H264Decoder] " << c.label
                      << " — element not available\n";
            return false;
        }

        GstStateChangeReturn scr = gst_element_set_state(p, GST_STATE_PAUSED);
        if (scr == GST_STATE_CHANGE_FAILURE) {
            std::cout << "[H264Decoder] " << c.label
                      << " — failed to reach PAUSED\n";
            gst_element_set_state(p, GST_STATE_NULL);
            gst_object_unref(p);
            return false;
        }

        pipeline = p;
        is_hw = c.hw;
        is_nv12 = c.nv12;

        if (as) appsrc  = as;
        else    appsrc  = gst_bin_get_by_name(GST_BIN(pipeline), "src");
        if (ak) appsink = ak;
        else    appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");

        std::cout << "[H264Decoder] selected: " << c.label << "\n";
        return true;
    }

    void build_pipeline() {
        static std::once_flag gst_once;
        std::call_once(gst_once, [] { gst_init(nullptr, nullptr); });

        // RGBA parse-launch strings for VA-API and software fallback
        const char* vaapi_pipe =
            "appsrc name=src is-live=true format=3 "
            "  caps=video/x-h264,stream-format=byte-stream,alignment=au ! "
            "h264parse ! vaapidecode ! vaapipostproc ! "
            "video/x-raw,format=RGBA ! "
            "appsink name=sink emit-signals=true sync=false drop=true max-buffers=1";

        const char* sw_pipe =
            "appsrc name=src is-live=true format=3 "
            "  caps=video/x-h264,stream-format=byte-stream,alignment=au ! "
            "h264parse ! decodebin ! videoconvert ! "
            "video/x-raw,format=RGBA ! "
            "appsink name=sink emit-signals=true sync=false drop=true max-buffers=1";

        const PipelineCandidate candidates[] = {
            { "VA-API HW (vaapidecode -> RGBA)", true,  false, nullptr,     vaapi_pipe },
            { "V4L2 stateless (v4l2slh264dec -> NV12)", true,  true,  build_v4l2sl, nullptr },
            { "V4L2 stateful (v4l2h264dec -> NV12)",    true,  true,  build_v4l2,   nullptr },
            { "auto/SW (decodebin -> RGBA)",     false, false, nullptr,     sw_pipe    },
        };

        for (const auto& c : candidates) {
            if (try_candidate(c)) break;
        }

        if (!pipeline)
            throw std::runtime_error("[H264Decoder] all pipeline candidates failed");
        if (!appsrc || !appsink)
            throw std::runtime_error("[H264Decoder] failed to get appsrc/appsink");

        // Connect appsink new-sample signal
        g_signal_connect(appsink, "new-sample",
                         G_CALLBACK(on_new_sample_static), this);
    }

    /* ── appsink callback (GStreamer thread) ── */

    static GstFlowReturn on_new_sample_static(GstElement*, gpointer data) {
        return static_cast<Impl*>(data)->on_new_sample();
    }

    GstFlowReturn on_new_sample() {
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
        if (!sample) return GST_FLOW_OK;

        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (!caps || !buffer || !buf) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        int w = 0, h = 0;
        bool have_vinfo = false;
        GstVideoInfo vinfo;
        memset(&vinfo, 0, sizeof(vinfo));

        // DMA_DRM format caps?  gst_video_info_from_caps can't parse them --
        // it returns garbage width/height/stride.  Extract w/h from the caps
        // structure directly and rely on GstVideoMeta for strides later.
        bool is_dma_drm = false;
        if (gst_caps_get_size(caps) > 0) {
            const GstStructure* s = gst_caps_get_structure(caps, 0);
            const gchar* fmt = gst_structure_get_string(s, "format");
            if (fmt && g_strcmp0(fmt, "DMA_DRM") == 0) {
                is_dma_drm = true;
                gst_structure_get_int(s, "width", &w);
                gst_structure_get_int(s, "height", &h);
            }
        }

        if (!is_dma_drm) {
            if (!gst_video_info_from_caps(&vinfo, caps)) {
                gst_sample_unref(sample);
                return GST_FLOW_OK;
            }
            have_vinfo = true;
            w = GST_VIDEO_INFO_WIDTH(&vinfo);
            h = GST_VIDEO_INFO_HEIGHT(&vinfo);
        }

        if (w <= 0 || h <= 0) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        // Pick a safe write slot -- whichever is NOT displaying
        FrameSlot* write_slot;
        {
            std::lock_guard<std::mutex> lock(buf->mutex);
            write_slot = (buf->displaying_slot == &buf->slots[0])
                       ? &buf->slots[1]
                       : &buf->slots[0];
        }

        // Close any stale DMA-BUF fds from previous frame in this slot
        write_slot->close_dma_fds();

        bool ok = false;
        // Check if GL thread signalled DMA-BUF import failure
        if (!dmabuf_runtime_disabled_cached && buf->dmabuf_import_failed.load(std::memory_order_relaxed)) {
            dmabuf_runtime_disabled_cached = true;
            std::cout << "[H264Decoder] DMA-BUF import failed on GL side; switching to NV12-CPU\n";
        }
        if (is_nv12 && !dmabuf_runtime_disabled_cached) {
            ok = try_extract_nv12_dmabuf(sample, buffer, caps,
                                         have_vinfo ? &vinfo : nullptr,
                                         w, h, write_slot);
        }
        if (!ok && is_nv12 && have_vinfo) {
            ok = try_extract_nv12_cpu(sample, buffer, &vinfo, w, h, write_slot);
        }
        // Fallback: map DMA-BUF directly when we have no vinfo (DMA_DRM caps)
        if (!ok && is_nv12 && !have_vinfo) {
            ok = try_extract_nv12_cpu_from_dmabuf(buffer, w, h, write_slot);
        }
        if (!ok && have_vinfo) {
            ok = try_extract_rgba(sample, buffer, &vinfo, w, h, write_slot);
        }

        gst_sample_unref(sample);

        if (!ok)
            return GST_FLOW_OK;

        // Atomic drop-and-replace
        buf->pending_slot.store(write_slot, std::memory_order_release);
        ++decode_count;

        if (decode_count == 1) {
            const char* fmt_str = "?";
            switch (write_slot->format) {
                case FrameFormat::kNV12_DMABUF: fmt_str = "NV12/DMABUF"; break;
                case FrameFormat::kNV12_CPU:    fmt_str = "NV12/CPU"; break;
                case FrameFormat::kRGBA:        fmt_str = "RGBA/CPU"; break;
                default: break;
            }
            std::cout << "[H264Decoder] first frame: "
                      << w << "x" << h << " " << fmt_str << "\n";
        }

        if (frame_ready_cb)
            frame_ready_cb();

        return GST_FLOW_OK;
    }

    /* ── NV12 DMA-BUF extraction ── */

    bool try_extract_nv12_dmabuf(GstSample* /*sample*/, GstBuffer* buffer,
                                 GstCaps* caps, GstVideoInfo* vinfo,
                                 int w, int h, FrameSlot* slot) {
        // Look for DMA-BUF memory objects
        int y_fd = -1, uv_fd = -1;
        guint nmem = gst_buffer_n_memory(buffer);
        for (guint i = 0; i < nmem; ++i) {
            GstMemory* mem = gst_buffer_peek_memory(buffer, i);
            if (!mem || !gst_is_dmabuf_memory(mem)) continue;
            int fd = gst_dmabuf_memory_get_fd(mem);
            if (fd < 0) continue;
            if (y_fd < 0) y_fd = fd;
            else if (uv_fd < 0) uv_fd = fd;
            if (y_fd >= 0 && uv_fd >= 0) break;
        }

        if (y_fd < 0)
            return false;
        if (uv_fd < 0) uv_fd = y_fd;  // single-fd NV12

        // Get plane layout -- prefer VideoMeta (always correct for DMA-BUF;
        // vinfo may be NULL or garbage for DMA_DRM caps).
        uint32_t y_stride  = 0;
        uint32_t uv_stride = 0;
        uint32_t y_offset  = 0;
        uint32_t uv_offset = 0;

        GstVideoMeta* vmeta = gst_buffer_get_video_meta(buffer);
        if (vmeta && vmeta->n_planes >= 2) {
            y_stride  = vmeta->stride[0];
            uv_stride = vmeta->stride[1];
            y_offset  = vmeta->offset[0];
            uv_offset = vmeta->offset[1];
        } else if (vinfo) {
            y_stride  = GST_VIDEO_INFO_PLANE_STRIDE(vinfo, 0);
            uv_stride = GST_VIDEO_INFO_PLANE_STRIDE(vinfo, 1);
            y_offset  = 0;
            uv_offset = y_stride * static_cast<uint32_t>(h);
        }

        // For separate fds, offsets are per-fd
        if (y_fd != uv_fd) {
            y_offset  = 0;
            uv_offset = 0;
        }

        // For single-fd NV12, ALWAYS try to infer stride from DMA-BUF size.
        // The BO pitch may be hardware-aligned (e.g. 64-byte) even though
        // VideoMeta reports the logical width as stride.  EGL import needs
        // the real BO pitch.  This matches cedrus_hw_h264's approach.
        if (y_fd == uv_fd) {
            GstMemory* y_mem = gst_buffer_peek_memory(buffer, 0);
            if (y_mem && h > 0) {
                gsize mem_offset = 0, mem_maxsize = 0;
                gst_memory_get_sizes(y_mem, &mem_offset, &mem_maxsize);
                uint64_t denom = static_cast<uint64_t>(h) * 3ull;
                if (denom > 0) {
                    uint64_t inferred = (static_cast<uint64_t>(mem_maxsize) * 2ull) / denom;
                    uint64_t total = inferred * static_cast<uint64_t>(h) * 3ull / 2ull;
                    if (inferred >= static_cast<uint64_t>(w) &&
                        (inferred & 0x3Full) == 0 &&
                        total <= static_cast<uint64_t>(mem_maxsize)) {
                        y_stride  = static_cast<uint32_t>(inferred);
                        uv_stride = static_cast<uint32_t>(inferred);
                        y_offset  = 0;
                        uv_offset = y_stride * static_cast<uint32_t>(h);
                    }
                }
            }
        }

        // Last resort: try frame-map to get strides (like cedrus does for
        // DMA_DRM caps where vinfo may be unusable)
        if (y_stride == 0 && vinfo) {
            GstVideoFrame vtmp;
            if (gst_video_frame_map(&vtmp, vinfo, buffer, GST_MAP_READ)) {
                y_stride  = GST_VIDEO_FRAME_PLANE_STRIDE(&vtmp, 0);
                uv_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vtmp, 1);
                y_offset  = 0;
                uv_offset = y_stride * static_cast<uint32_t>(h);
                gst_video_frame_unmap(&vtmp);
            }
        }

        if (y_stride == 0 || uv_stride == 0)
            return false;



        // Dup the fds so the slot owns them independently of GStreamer buffer lifetime
        slot->format        = FrameFormat::kNV12_DMABUF;
        slot->width         = w;
        slot->height        = h;
        slot->y_dma_fd      = dup(y_fd);
        slot->uv_dma_fd     = (uv_fd == y_fd) ? slot->y_dma_fd : dup(uv_fd);
        slot->y_dma_offset  = y_offset;
        slot->uv_dma_offset = uv_offset;
        slot->y_dma_stride  = y_stride;
        slot->uv_dma_stride = uv_stride;
        slot->dirty         = true;

        ++dmabuf_count;
        return true;
    }

    /* ── NV12 CPU extraction ── */

    bool try_extract_nv12_cpu(GstSample* /*sample*/, GstBuffer* buffer,
                              GstVideoInfo* vinfo, int w, int h,
                              FrameSlot* slot) {
        GstVideoFrame vframe;
        if (!gst_video_frame_map(&vframe, vinfo, buffer, GST_MAP_READ))
            return false;

        int n_planes = GST_VIDEO_FRAME_N_PLANES(&vframe);
        if (n_planes < 2) {
            gst_video_frame_unmap(&vframe);
            return false;
        }

        // Y plane
        const int y_stride_src = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
        const uint8_t* y_src = static_cast<const uint8_t*>(
            GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0));

        // UV plane
        const int uv_w = (w + 1) / 2;
        const int uv_h = (h + 1) / 2;
        const int uv_stride_src = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1);
        const uint8_t* uv_src = static_cast<const uint8_t*>(
            GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1));

        // Copy Y plane
        const size_t y_row = static_cast<size_t>(w);
        slot->y_plane.resize(static_cast<size_t>(w) * h);
        for (int row = 0; row < h; ++row) {
            memcpy(slot->y_plane.data() + row * y_row,
                   y_src + row * y_stride_src, y_row);
        }

        // Copy UV plane (interleaved, uv_w*2 bytes per row)
        const size_t uv_row = static_cast<size_t>(uv_w) * 2;
        slot->uv_plane.resize(uv_row * uv_h);
        for (int row = 0; row < uv_h; ++row) {
            memcpy(slot->uv_plane.data() + row * uv_row,
                   uv_src + row * uv_stride_src, uv_row);
        }

        gst_video_frame_unmap(&vframe);

        slot->format    = FrameFormat::kNV12_CPU;
        slot->width     = w;
        slot->height    = h;
        slot->y_stride  = w;
        slot->uv_stride = static_cast<int>(uv_row);
        slot->dirty     = true;

        ++nv12_cpu_count;
        return true;
    }

    /* ── NV12 CPU extraction from DMA-BUF (for DMA_DRM caps w/o vinfo) ── */
    /// Maps a DMA-BUF buffer directly via gst_buffer_map() and copies out
    /// Y+UV planes using VideoMeta stride/offset.  This path is used when
    /// EGL DMA-BUF import fails and gst_video_info_from_caps() cannot parse
    /// DMA_DRM caps (have_vinfo=false).
    bool try_extract_nv12_cpu_from_dmabuf(GstBuffer* buffer,
                                          int w, int h,
                                          FrameSlot* slot) {
        // We NEED VideoMeta for stride/offset info
        GstVideoMeta* vmeta = gst_buffer_get_video_meta(buffer);
        if (!vmeta || vmeta->n_planes < 2)
            return false;

        const uint32_t y_stride_src  = vmeta->stride[0];
        const uint32_t uv_stride_src = vmeta->stride[1];
        const gsize    y_offset      = vmeta->offset[0];
        const gsize    uv_offset     = vmeta->offset[1];

        if (y_stride_src == 0 || uv_stride_src == 0)
            return false;

        // Map the buffer for CPU read access (GStreamer mmap's the DMA-BUF fd)
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
            return false;

        const int uv_w = (w + 1) / 2;
        const int uv_h = (h + 1) / 2;

        // Sanity check: buffer must be large enough
        const gsize y_end  = y_offset + static_cast<gsize>(y_stride_src) * (h - 1) + w;
        const gsize uv_end = uv_offset + static_cast<gsize>(uv_stride_src) * (uv_h - 1) + uv_w * 2;
        const gsize need   = (y_end > uv_end) ? y_end : uv_end;
        if (map.size < need) {
            gst_buffer_unmap(buffer, &map);
            return false;
        }

        const uint8_t* base = map.data;

        // Copy Y plane
        const size_t y_row = static_cast<size_t>(w);
        slot->y_plane.resize(static_cast<size_t>(w) * h);
        for (int row = 0; row < h; ++row) {
            memcpy(slot->y_plane.data() + row * y_row,
                   base + y_offset + row * y_stride_src, y_row);
        }

        // Copy UV plane (interleaved, uv_w*2 bytes per row)
        const size_t uv_row = static_cast<size_t>(uv_w) * 2;
        slot->uv_plane.resize(uv_row * uv_h);
        for (int row = 0; row < uv_h; ++row) {
            memcpy(slot->uv_plane.data() + row * uv_row,
                   base + uv_offset + row * uv_stride_src, uv_row);
        }

        gst_buffer_unmap(buffer, &map);

        slot->format    = FrameFormat::kNV12_CPU;
        slot->width     = w;
        slot->height    = h;
        slot->y_stride  = w;
        slot->uv_stride = static_cast<int>(uv_row);
        slot->dirty     = true;

        ++nv12_cpu_count;
        return true;
    }

    /* ── RGBA CPU extraction (fallback) ── */

    bool try_extract_rgba(GstSample* /*sample*/, GstBuffer* buffer,
                          GstVideoInfo* vinfo, int w, int h,
                          FrameSlot* slot) {
        GstVideoFrame vframe;
        if (!gst_video_frame_map(&vframe, vinfo, buffer, GST_MAP_READ))
            return false;

        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
        const size_t rgba_size = static_cast<size_t>(w) * h * 4;
        slot->pixels.resize(rgba_size);
        slot->width   = w;
        slot->height  = h;
        slot->stride  = w * 4;

        const uint8_t* src = static_cast<const uint8_t*>(
            GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0));
        uint8_t* dst = slot->pixels.data();
        const int row_bytes = w * 4;
        if (stride == row_bytes) {
            memcpy(dst, src, rgba_size);
        } else {
            for (int row = 0; row < h; ++row) {
                memcpy(dst + row * row_bytes,
                       src + row * stride, row_bytes);
            }
        }

        gst_video_frame_unmap(&vframe);

        slot->format = FrameFormat::kRGBA;
        slot->dirty  = true;

        ++rgba_count;
        return true;
    }

    /* ── bus drain ── */

    void drain_bus() {
        GstBus* bus = gst_element_get_bus(pipeline);
        if (!bus) return;
        GstMessage* msg;
        while ((msg = gst_bus_pop(bus)) != nullptr) {
            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_ERROR: {
                    GError* gerr = nullptr; gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &gerr, &dbg);
                    std::cerr << "[H264Decoder] BUS ERROR: "
                              << (gerr ? gerr->message : "?")
                              << " | " << (dbg ? dbg : "") << std::endl;
                    if (gerr) g_error_free(gerr);
                    g_free(dbg);
                    break;
                }
                case GST_MESSAGE_WARNING: {
                    GError* gerr = nullptr; gchar* dbg = nullptr;
                    gst_message_parse_warning(msg, &gerr, &dbg);
                    std::cerr << "[H264Decoder] BUS WARNING: "
                              << (gerr ? gerr->message : "?")
                              << " | " << (dbg ? dbg : "") << std::endl;
                    if (gerr) g_error_free(gerr);
                    g_free(dbg);
                    break;
                }
                default:
                    break;
            }
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    }

    /* ── lifecycle ── */

    ~Impl() {
        std::cout << "[H264Decoder] shutting down — pushed=" << push_count
                  << " decoded=" << decode_count << " failed=" << fail_count
                  << " (dmabuf=" << dmabuf_count << " nv12cpu=" << nv12_cpu_count
                  << " rgba=" << rgba_count << ")\n";
        if (pipeline) {
            if (appsrc)
                gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
        if (appsrc)  gst_object_unref(appsrc);
        if (appsink) gst_object_unref(appsink);
        if (pipeline) gst_object_unref(pipeline);
    }
};

/* ------------------------------------------------------------------ */
/*  FrameSlot helpers                                                 */
/* ------------------------------------------------------------------ */

void FrameSlot::close_dma_fds() {
    if (y_dma_fd >= 0) {
        if (uv_dma_fd >= 0 && uv_dma_fd != y_dma_fd)
            close(uv_dma_fd);
        close(y_dma_fd);
        y_dma_fd = -1;
        uv_dma_fd = -1;
    }
    format = FrameFormat::kNone;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

H264Decoder::H264Decoder() : impl_(new Impl()) {
    impl_->build_pipeline();
}

H264Decoder::~H264Decoder() {
    delete impl_;
}

void H264Decoder::set_buffer(DropBuffer* buf) {
    impl_->buf = buf;
}

void H264Decoder::set_frame_ready_callback(FrameReadyCallback cb) {
    impl_->frame_ready_cb = std::move(cb);
}

void H264Decoder::start() {
    if (impl_->started) return;
    GstStateChangeReturn scr = gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING);
    if (scr == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[H264Decoder] failed to start pipeline\n";
        return;
    }
    impl_->started = true;
    std::cout << "[H264Decoder] pipeline PLAYING (hw=" << impl_->is_hw
              << " nv12=" << impl_->is_nv12 << ")\n";
}

void H264Decoder::stop() {
    if (!impl_->started) return;
    if (impl_->appsrc)
        gst_app_src_end_of_stream(GST_APP_SRC(impl_->appsrc));
    gst_element_set_state(impl_->pipeline, GST_STATE_NULL);
    impl_->started = false;
    std::cout << "[H264Decoder] pipeline stopped\n";
}

void H264Decoder::push_h264(const uint8_t* data, size_t size) {
    if (!impl_->started || !data || size == 0)
        return;

    ++impl_->push_count;
    impl_->drain_bus();

    GstBuffer* gst_buf = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_fill(gst_buf, 0, data, size);
    GST_BUFFER_PTS(gst_buf) = GST_CLOCK_TIME_NONE;

    GstFlowReturn fr = gst_app_src_push_buffer(GST_APP_SRC(impl_->appsrc), gst_buf);
    if (fr != GST_FLOW_OK) {
        std::cerr << "[H264Decoder] appsrc push FAILED: " << gst_flow_get_name(fr) << "\n";
        ++impl_->fail_count;
    }
}
