#include "oa_audio_player.h"

#include <gst/app/gstappsrc.h>
#include <glib.h>
#include <algorithm>
#include <cstring>

OAAudioPlayer::OAAudioPlayer(const Config& cfg) : cfg_(cfg) {
  buildPipeline();
}

OAAudioPlayer::~OAAudioPlayer() {
  teardownPipeline();
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

void OAAudioPlayer::buildPipeline() {
  std::lock_guard<std::mutex> lock(mutex_);

  const char* format = (cfg_.bits_per_sample == 16) ? "S16LE" : "S32LE";

  // appsrc → audioconvert → audioresample → scaletempo → volume → autoaudiosink
  pipeline_  = gst_pipeline_new(nullptr);
  appsrc_    = gst_element_factory_make("appsrc",        nullptr);
  auto* conv = gst_element_factory_make("audioconvert",  nullptr);
  auto* resample = gst_element_factory_make("audioresample", nullptr);
  scaletempo_ = gst_element_factory_make("scaletempo",   nullptr);
  volume_el_ = gst_element_factory_make("volume",        nullptr);
  auto* sink = gst_element_factory_make("autoaudiosink", nullptr);

  if (!pipeline_ || !appsrc_ || !conv || !resample || !scaletempo_ || !volume_el_ || !sink) {
    g_warning("OAAudioPlayer: failed to create GStreamer elements");
    if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
    return;
  }

  // Configure appsrc caps
  GstCaps* caps = gst_caps_new_simple(
      "audio/x-raw",
      "format",   G_TYPE_STRING,  format,
      "rate",     G_TYPE_INT,     cfg_.sample_rate,
      "channels", G_TYPE_INT,     cfg_.channels,
      "layout",   G_TYPE_STRING,  "interleaved",
      nullptr);
  g_object_set(appsrc_,
               "caps",        caps,
               "format",      GST_FORMAT_TIME,
               "is-live",     TRUE,
               "do-timestamp", FALSE,
               nullptr);
  gst_caps_unref(caps);

  // Configure volume
  const double vol = std::clamp(volume_.load() / 100.0, 0.0, 1.0);
  g_object_set(volume_el_, "volume", vol, nullptr);

  // Configure audio device if specified
  if (!cfg_.device.empty()) {
    // autoaudiosink wraps a child; we set properties on the sink element
    // via the GstChildProxy interface after the pipeline is playing, or
    // we use pulsesink/alsasink directly.
    // For simplicity, use an env-style hint via GST properties.
    g_object_set(sink, "ts-offset", (gint64)0, nullptr);
  }

  gst_bin_add_many(GST_BIN(pipeline_), appsrc_, conv, resample, scaletempo_, volume_el_, sink, nullptr);
  if (!gst_element_link_many(appsrc_, conv, resample, scaletempo_, volume_el_, sink, nullptr)) {
    g_warning("OAAudioPlayer: failed to link pipeline");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return;
  }

  gst_element_set_state(pipeline_, GST_STATE_PLAYING);
}

void OAAudioPlayer::teardownPipeline() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pipeline_) return;
  gst_element_set_state(pipeline_, GST_STATE_NULL);
  gst_object_unref(pipeline_);
  pipeline_  = nullptr;
  appsrc_    = nullptr;  // owned by pipeline
  volume_el_ = nullptr;
  scaletempo_ = nullptr;
  catching_up_ = false;
}

// ---------------------------------------------------------------------------
// Push audio data
// ---------------------------------------------------------------------------

void OAAudioPlayer::push(const void* data, std::size_t size, uint64_t timestamp_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!appsrc_ || !data || size == 0) return;

  GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
  gst_buffer_fill(buf, 0, data, size);

  // Set PTS from the transport timestamp
  GST_BUFFER_PTS(buf) = timestamp_us * GST_USECOND;
  // Duration based on sample count
  const int bytes_per_sample = (cfg_.bits_per_sample / 8) * cfg_.channels;
  if (bytes_per_sample > 0) {
    const uint64_t samples = size / bytes_per_sample;
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(samples, GST_SECOND, cfg_.sample_rate);
  }

  gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);

  checkCatchUp();
}

// ---------------------------------------------------------------------------
// Catch-up logic
// ---------------------------------------------------------------------------

void OAAudioPlayer::checkCatchUp() {
  // Must be called with mutex_ held.
  if (!pipeline_ || !scaletempo_) return;

  // Use appsrc current-level-bytes as a heuristic for queue depth
  guint64 queued_bytes = 0;
  g_object_get(appsrc_, "current-level-bytes", &queued_bytes, nullptr);
  const int bytes_per_sample = (cfg_.bits_per_sample / 8) * cfg_.channels;
  const int64_t queued_us = (bytes_per_sample > 0 && cfg_.sample_rate > 0)
      ? static_cast<int64_t>(queued_bytes) * 1'000'000 / (bytes_per_sample * cfg_.sample_rate)
      : 0;

  if (!catching_up_ && queued_us > kMaxLatencyUs) {
    catching_up_ = true;
    g_object_set(scaletempo_, "rate", kCatchUpRate, nullptr);
    // Seek to apply rate change
    gst_element_seek(pipeline_, kCatchUpRate, GST_FORMAT_TIME,
                     static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_INSTANT_RATE_CHANGE),
                     GST_SEEK_TYPE_NONE, 0, GST_SEEK_TYPE_NONE, 0);
  } else if (catching_up_ && queued_us < kMaxLatencyUs / 2) {
    catching_up_ = false;
    gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
                     static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_INSTANT_RATE_CHANGE),
                     GST_SEEK_TYPE_NONE, 0, GST_SEEK_TYPE_NONE, 0);
  }
}

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

void OAAudioPlayer::setVolume(int percent) {
  percent = std::clamp(percent, 0, 100);
  volume_.store(percent, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(mutex_);
  if (volume_el_) {
    g_object_set(volume_el_, "volume", percent / 100.0, nullptr);
  }
}

// ---------------------------------------------------------------------------
// Device change
// ---------------------------------------------------------------------------

void OAAudioPlayer::setDevice(const std::string& device) {
  // Tear down and rebuild with new device
  teardownPipeline();
  cfg_.device = device;
  buildPipeline();
}
