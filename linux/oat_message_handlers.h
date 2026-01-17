#pragma once

#include "transport.hpp"
#include "wire.hpp"
#include "av/oa_video_texture.h"
#include "av/h264_decoder.h"

#include <flutter_linux/flutter_linux.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using NativeTransport = buzz::autoapp::Transport::Transport;

class OatMessageHandlers {
public:
  OatMessageHandlers(NativeTransport& transport,
                     OAVideoTexture* texture,
                     FlTextureRegistrar* registrar);

  void install();
  bool handleTouchMethod(FlValue* args, std::string& error);
  bool handleSensorMethod(FlValue* args, std::string& error);

private:
  struct VideoState {
    std::shared_ptr<H264Decoder> decoder;
    std::mutex mutex;
    std::vector<uint8_t> yuv;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    bool warned_missing_headers = false;
  };

  static std::string hexHead(const uint8_t* data, std::size_t size, std::size_t max_bytes = 32);
  static void logPayload(const char* label, uint64_t envelope_ts, const void* data, std::size_t size);

  void handleVideo(uint64_t envelope_ts, const void* data, std::size_t size);
  void handleSimple(buzz::wire::MsgType type, uint64_t envelope_ts, const void* data, std::size_t size);

  NativeTransport& transport_;
  OAVideoTexture* texture_;
  FlTextureRegistrar* registrar_;
  std::shared_ptr<VideoState> video_state_;
  bool installed_;
};
