#pragma once

#include <gst/gst.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

/// Real-time PCM audio player using GStreamer.
///
/// appsrc → queue → audioconvert → audioresample → volume → autoaudiosink
/// sync=false — plays audio immediately, no clock sync needed.
/// Queue size adapts dynamically based on frame arrival jitter.
class OAAudioPlayer {
public:
  struct Config {
    int sample_rate     = 48000;
    int channels        = 2;
    int bits_per_sample = 16;
    std::string device;
  };

  explicit OAAudioPlayer(const Config& cfg);
  ~OAAudioPlayer();

  OAAudioPlayer(const OAAudioPlayer&) = delete;
  OAAudioPlayer& operator=(const OAAudioPlayer&) = delete;

  void push(const void* data, std::size_t size, uint64_t timestamp_us);
  void setVolume(int percent);
  int  volume() const { return volume_.load(std::memory_order_relaxed); }
  void setDevice(const std::string& device);

private:
  void buildPipeline();
  void teardownPipeline();

  Config cfg_;
  std::atomic<int> volume_{100};
  std::mutex mutex_;

  GstElement* pipeline_  = nullptr;
  GstElement* appsrc_    = nullptr;
  GstElement* queue_     = nullptr;
  GstElement* volume_el_ = nullptr;

  // Dynamic queue sizing
  int64_t last_push_us_ = 0;
  int64_t jitter_avg_us_ = 0;
  static constexpr int64_t kQueueMinUs =    10'000;   // 10 ms
  static constexpr int64_t kQueueMaxUs = 1'000'000;   // 1000 ms
};
