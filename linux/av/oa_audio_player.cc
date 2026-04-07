#include "oa_audio_player.h"

#include <gst/app/gstappsrc.h>
#include <glib.h>
#include <algorithm>

OAAudioPlayer::OAAudioPlayer(const Config& cfg) : cfg_(cfg) {
  buildPipeline();
}

OAAudioPlayer::~OAAudioPlayer() {
  teardownPipeline();
}

void OAAudioPlayer::buildPipeline() {
  std::lock_guard<std::mutex> lock(mutex_);

  const char* format = (cfg_.bits_per_sample == 16) ? "S16LE" : "S32LE";

  pipeline_  = gst_pipeline_new(nullptr);
  appsrc_    = gst_element_factory_make("appsrc",          nullptr);
  queue_     = gst_element_factory_make("queue",           nullptr);
  auto* conv = gst_element_factory_make("audioconvert",    nullptr);
  auto* resample = gst_element_factory_make("audioresample", nullptr);
  volume_el_ = gst_element_factory_make("volume",          nullptr);
  auto* sink = gst_element_factory_make("autoaudiosink",   nullptr);

  if (!pipeline_ || !appsrc_ || !queue_ || !conv || !resample || !volume_el_ || !sink) {
    g_warning("OAAudioPlayer: failed to create elements");
    if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
    return;
  }

  GstCaps* caps = gst_caps_new_simple(
      "audio/x-raw",
      "format",   G_TYPE_STRING, format,
      "rate",     G_TYPE_INT,    cfg_.sample_rate,
      "channels", G_TYPE_INT,    cfg_.channels,
      "layout",   G_TYPE_STRING, "interleaved",
      nullptr);
  g_object_set(appsrc_,
               "caps",         caps,
               "format",       GST_FORMAT_TIME,
               "is-live",      TRUE,
               "do-timestamp", TRUE,
               nullptr);
  gst_caps_unref(caps);

  g_object_set(queue_,
               "max-size-time",    (guint64)(500 * GST_MSECOND),
               "max-size-buffers", 0,
               "max-size-bytes",   0,
               nullptr);

  g_object_set(volume_el_, "volume",
               std::clamp(volume_.load() / 100.0, 0.0, 1.0), nullptr);
  g_object_set(sink, "sync", FALSE, nullptr);

  gst_bin_add_many(GST_BIN(pipeline_),
                   appsrc_, queue_, conv, resample, volume_el_, sink, nullptr);
  if (!gst_element_link_many(appsrc_, queue_, conv, resample, volume_el_, sink, nullptr)) {
    g_warning("OAAudioPlayer: failed to link pipeline");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return;
  }

  gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  last_push_us_  = 0;
  jitter_avg_us_ = 0;

  g_message("OAAudioPlayer[%dHz/%dch]: started", cfg_.sample_rate, cfg_.channels);
}

void OAAudioPlayer::teardownPipeline() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pipeline_) return;
  gst_element_set_state(pipeline_, GST_STATE_NULL);
  gst_object_unref(pipeline_);
  pipeline_  = nullptr;
  appsrc_    = nullptr;
  queue_     = nullptr;
  volume_el_ = nullptr;
}

void OAAudioPlayer::push(const void* data, std::size_t size, uint64_t /*timestamp_us*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!appsrc_ || !data || size == 0) return;

  // Adapt queue size based on arrival jitter
  int64_t now_us = g_get_monotonic_time();
  if (last_push_us_ > 0) {
    int64_t delta = now_us - last_push_us_;
    jitter_avg_us_ += (delta - jitter_avg_us_) / 16;  // EMA α=1/16
    int64_t target_us = std::clamp(jitter_avg_us_ * 4, kQueueMinUs, kQueueMaxUs);
    g_object_set(queue_, "max-size-time", (guint64)(target_us * 1000), nullptr);
  }
  last_push_us_ = now_us;

  GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
  gst_buffer_fill(buf, 0, data, size);
  gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
}

void OAAudioPlayer::setVolume(int percent) {
  percent = std::clamp(percent, 0, 100);
  volume_.store(percent, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mutex_);
  if (volume_el_)
    g_object_set(volume_el_, "volume", percent / 100.0, nullptr);
}

void OAAudioPlayer::setDevice(const std::string& device) {
  teardownPipeline();
  cfg_.device = device;
  buildPipeline();
}
