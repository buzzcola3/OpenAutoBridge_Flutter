#pragma once

#include <gst/gst.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

/// Real-time PCM audio player using GStreamer.
///
/// Pushes raw PCM buffers into an `appsrc → audioconvert → audioresample
/// → scaletempo → autoaudiosink` pipeline. When the internal queue grows
/// beyond a latency threshold the playback rate is temporarily raised to
/// catch up, then returns to 1.0×.
class OAAudioPlayer {
public:
  struct Config {
    int sample_rate    = 48000;
    int channels       = 2;
    int bits_per_sample = 16;
    std::string device;           // ALSA/Pulse device name (empty = default)
  };

  explicit OAAudioPlayer(const Config& cfg);
  ~OAAudioPlayer();

  OAAudioPlayer(const OAAudioPlayer&) = delete;
  OAAudioPlayer& operator=(const OAAudioPlayer&) = delete;

  /// Push PCM data into the pipeline. Thread-safe.
  void push(const void* data, std::size_t size, uint64_t timestamp_us);

  /// Volume 0–100. Thread-safe.
  void setVolume(int percent);
  int  volume() const { return volume_.load(std::memory_order_relaxed); }

  /// Change audio output device. Restarts pipeline.
  void setDevice(const std::string& device);

private:
  void buildPipeline();
  void teardownPipeline();
  void checkCatchUp();

  Config cfg_;
  std::atomic<int> volume_{100};
  std::mutex mutex_;

  GstElement* pipeline_  = nullptr;
  GstElement* appsrc_    = nullptr;
  GstElement* volume_el_ = nullptr;
  GstElement* scaletempo_ = nullptr;

  // Catch-up state
  static constexpr int64_t kMaxLatencyUs = 150'000;  // 150 ms
  static constexpr double  kCatchUpRate  = 1.05;
  bool catching_up_ = false;
};
