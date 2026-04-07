#pragma once

#include "transport.hpp"
#include "wire.hpp"
#include "av/oa_video_texture.h"
#include "av/h264_decoder.h"
#include "av/oa_audio_player.h"

#include <flutter_linux/flutter_linux.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

using NativeTransport = buzz::autoapp::Transport::Transport;

class OatMessageHandlers {
public:
  OatMessageHandlers(NativeTransport& transport,
                     OAVideoTexture* texture,
                     FlTextureRegistrar* registrar,
                     DropBuffer* drop_buffer,
                     FlMethodChannel* channel);

  void install();
  bool handleTouchMethod(FlValue* args, std::string& error);
  bool handleSensorMethod(FlValue* args, std::string& error);
  bool handleConfigMethod(FlValue* args, std::string& error);
  bool handleControlMethod(FlValue* args, std::string& error);
  bool handleAudioVolumeMethod(FlValue* args, std::string& error);
  bool handleAudioDeviceMethod(FlValue* args, std::string& error);

  /// Access the decoder (e.g. for start/stop lifecycle control).
  H264Decoder& decoder() { return decoder_; }

private:
  static std::string hexHead(const uint8_t* data, std::size_t size, std::size_t max_bytes = 32);

  void handleVideo(uint64_t envelope_ts, const void* data, std::size_t size);
  void handleAudio(buzz::wire::MsgType type, uint64_t ts, const void* data, std::size_t size);
  void handleConfigResponse(uint64_t envelope_ts, const void* data, std::size_t size);
  void handleControlResponse(uint64_t envelope_ts, const void* data, std::size_t size);

  NativeTransport& transport_;
  OAVideoTexture* texture_;
  FlTextureRegistrar* registrar_;
  DropBuffer* drop_buffer_;
  FlMethodChannel* channel_;
  H264Decoder decoder_;
  bool installed_;

  // Audio players — one per channel
  std::unique_ptr<OAAudioPlayer> media_audio_;     // 48kHz stereo
  std::unique_ptr<OAAudioPlayer> guidance_audio_;   // 16kHz mono
  std::unique_ptr<OAAudioPlayer> system_audio_;     // 16kHz mono
};
