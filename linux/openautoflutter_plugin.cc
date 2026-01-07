#include "include/openautoflutter/openautoflutter_plugin.h"
#include "av/oa_video_texture.h"
#include "av/h264_decoder.h"
#include "transport.hpp"
#include "oat_transport.hpp"
#include "wire.hpp"
#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <glib-object.h>
#include <glib.h>
#include <sys/utsname.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>
#include <string>

using OAMsgType = buzz::wire::MsgType;

#include "openautoflutter_plugin_private.h"

namespace {
// Short hex preview helper for debugging payloads.
std::string hex_head(const uint8_t* data, size_t size, size_t max_bytes = 32) {
  if (!data || size == 0) return "";
  std::ostringstream oss;
  const size_t n = std::min(size, max_bytes);
  for (size_t i = 0; i < n; ++i) {
    if (i) oss << ' ';
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
  }
  if (size > max_bytes) oss << " ...";
  return oss.str();
}

enum class TouchAction : uint32_t {
  DOWN = 0,
  UP = 1,
  MOVED = 2,
  POINTER_DOWN = 3,
  POINTER_UP = 4,
};

struct TouchMessage {
  float x;
  float y;
  uint32_t pointer_id;
  uint32_t action;
};

static_assert(sizeof(TouchMessage) == 16, "TouchMessage layout unexpected");

double get_number(FlValue* value, bool& ok) {
  ok = false;
  if (!value) return 0.0;
  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_INT:
      ok = true;
      return static_cast<double>(fl_value_get_int(value));
    case FL_VALUE_TYPE_FLOAT:
      ok = true;
      return fl_value_get_float(value);
    default:
      return 0.0;
  }
}

bool parse_touch_args(FlValue* args, TouchMessage& out, std::string& error) {
  if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    error = "Args must be a map";
    return false;
  }

  bool ok = false;
  const double x = get_number(fl_value_lookup_string(args, "x"), ok);
  if (!ok) {
    error = "Missing or invalid x";
    return false;
  }
  const double y = get_number(fl_value_lookup_string(args, "y"), ok);
  if (!ok) {
    error = "Missing or invalid y";
    return false;
  }
  const double pid_val = get_number(fl_value_lookup_string(args, "pointerId"), ok);
  if (!ok) {
    error = "Missing or invalid pointerId";
    return false;
  }
  const double action_val = get_number(fl_value_lookup_string(args, "action"), ok);
  if (!ok) {
    error = "Missing or invalid action";
    return false;
  }

  const int64_t action_i = static_cast<int64_t>(action_val);
  TouchAction action_enum;
  switch (action_i) {
    case 0: action_enum = TouchAction::DOWN; break;
    case 1: action_enum = TouchAction::UP; break;
    case 2: action_enum = TouchAction::MOVED; break;
    case 3: action_enum = TouchAction::POINTER_DOWN; break;
    case 4: action_enum = TouchAction::POINTER_UP; break;
    default:
      error = "Unsupported action code";
      return false;
  }

  const auto clamp01 = [](double v) {
    return std::clamp(v, 0.0, 1.0);
  };

  out.x = static_cast<float>(clamp01(x));
  out.y = static_cast<float>(clamp01(y));
  out.pointer_id = pid_val < 0 ? 0u : static_cast<uint32_t>(pid_val);
  out.action = static_cast<uint32_t>(action_enum);
  return true;
}
} // namespace

#define OPENAUTOFLUTTER_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), openautoflutter_plugin_get_type(), \
                              OpenautoflutterPlugin))

struct _OpenautoflutterPlugin {
  GObject parent_instance;
  OAVideoTexture* video_texture;
  int64_t texture_id;
  std::unique_ptr<OatTransport> transport; // OpenAutoTransport receiver
  std::unique_ptr<OatMessageLogger> message_logger;
  std::shared_ptr<H264Decoder> decoder; // shared to keep alive beyond plugin lifetime

  struct VideoFrameState {
    std::mutex mutex;
    std::vector<uint8_t> yuv; // packed YUV420P [Y][U][V]
    int width = 0;
    int height = 0;
    int64_t recv_ts_us = 0;   // when handler received packet
    int64_t decode_ts_us = 0; // when decode completed
    bool has_new = false;

    std::atomic<int> log_count{0};

    // Extract payload (optionally strip 8-byte ts + 4-byte payload header) and decode.
    void ingest_packet(const uint8_t* data, std::size_t size, H264Decoder& decoder) {
      if (!data || size == 0) return;

      const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();

      const uint8_t* payload = data;
      std::size_t payload_size = size;
      bool stripped = false;
      uint32_t declared = 0;

      // Helper: find first Annex-B start code (3 or 4 bytes).
      auto find_start_code = [](const uint8_t* p, size_t n) -> size_t {
        for (size_t i = 0; i + 3 < n; ++i) {
          if (p[i] == 0 && p[i + 1] == 0 && ((p[i + 2] == 1) || (p[i + 2] == 0 && p[i + 3] == 1))) {
            return i;
          }
        }
        return n;
      };

      // Common OAT framing: [u64 ts][u32 payload_size][payload...]
      if (size >= sizeof(uint64_t) + sizeof(uint32_t)) {
        std::memcpy(&declared, data + sizeof(uint64_t), sizeof(uint32_t));
        if (declared > 0 && declared <= size - (sizeof(uint64_t) + sizeof(uint32_t))) {
          payload = data + sizeof(uint64_t) + sizeof(uint32_t);
          payload_size = declared;
          stripped = true;
        }
      }

      // If not stripped yet, try to drop any leading non-start-code bytes (e.g., 4-byte length + nonce).
      if (!stripped) {
        size_t idx = find_start_code(payload, payload_size);
        if (idx != 0 && idx < payload_size) {
          payload += idx;
          payload_size -= idx;
          stripped = true;
        }
      }

      int log_id = log_count.fetch_add(1, std::memory_order_relaxed);
      if (log_id < 8) {
        std::cout << "[VideoFrameState] in_size=" << size
                  << " payload_size=" << payload_size
                  << " stripped=" << (stripped ? 1 : 0)
                  << " head=" << hex_head(payload, payload_size, 24)
                  << std::endl;
      }

      int w = 0, h = 0;
      std::vector<uint8_t> decoded;
      if (!decoder.decode_to_yuv420p(payload, payload_size, decoded, w, h)) {
        if (log_id < 8) {
          std::cout << "[VideoFrameState] decode failed size=" << payload_size
                    << " declared=" << declared << std::endl;
        }
        return;
      }
      const auto decode_end_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();

      std::lock_guard<std::mutex> lk(mutex);
      width = w;
      height = h;
      yuv.swap(decoded);
      recv_ts_us = now_us;
      decode_ts_us = decode_end_us;
      has_new = true;
      if (log_id < 8) {
        std::cout << "[VideoFrameState] decoded " << w << "x" << h
                  << " bytes=" << yuv.size() << std::endl;
      }
    }

    bool take_latest(std::vector<uint8_t>& out, int& w, int& h, int64_t& recv_us, int64_t& dec_us) {
      std::lock_guard<std::mutex> lk(mutex);
      if (!has_new || yuv.empty() || width <= 0 || height <= 0) return false;
      out = yuv; // copy to avoid holding lock during GL upload
      w = width;
      h = height;
      recv_us = recv_ts_us;
      dec_us = decode_ts_us;
      has_new = false;
      recv_ts_us = 0;
      decode_ts_us = 0;
      return true;
    }
  };

  std::shared_ptr<VideoFrameState> frame_state;
  FlTextureRegistrar* texture_registrar; // to mark frames available
  guint frame_timer_id; // periodic pump for decoded frames
};

// Convenience alias for the nested frame holder type.
using VideoFrameState = _OpenautoflutterPlugin::VideoFrameState;

G_DEFINE_TYPE(OpenautoflutterPlugin, openautoflutter_plugin, g_object_get_type())

// Called when a method call is received from Flutter.
static void openautoflutter_plugin_handle_method_call(
    OpenautoflutterPlugin* self,
    FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getPlatformVersion") == 0) {
    response = get_platform_version();
  } else if (strcmp(method, "getVideoTextureId") == 0) {
    // Return the registered Flutter texture ID so Dart can render it via a Texture widget.
    g_autoptr(FlValue) result = fl_value_new_int(self->texture_id);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else if (strcmp(method, "sendTouchEvent") == 0) {
    TouchMessage touch_msg{};
    std::string error;
    if (!parse_touch_args(fl_method_call_get_args(method_call), touch_msg, error)) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("invalid_args", error.c_str(), nullptr));
    } else {
      const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      if (self->transport && self->transport->isRunning()) {
        self->transport->send(OAMsgType::TOUCH, static_cast<uint64_t>(now_us), &touch_msg, sizeof(touch_msg));
      } else {
        g_warning("OAT: transport not running; dropping touch event");
      }
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    }
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

FlMethodResponse* get_platform_version() {
  struct utsname uname_data = {};
  uname(&uname_data);
  g_autofree gchar *version = g_strdup_printf("Linux %s", uname_data.version);
  g_autoptr(FlValue) result = fl_value_new_string(version);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static void openautoflutter_plugin_dispose(GObject* object) {
  OpenautoflutterPlugin* self = OPENAUTOFLUTTER_PLUGIN(object);
  if (self->frame_timer_id) {
    g_source_remove(self->frame_timer_id);
    self->frame_timer_id = 0;
  }
  if (self->transport) {
    self->message_logger.reset();
    self->transport->clearStatusCallback();
    self->transport->stop();
    self->transport.reset();
  }
  if (self->video_texture != nullptr) {
    g_clear_object(&self->video_texture);
  }
  self->decoder.reset();
  G_OBJECT_CLASS(openautoflutter_plugin_parent_class)->dispose(object);
}

static void openautoflutter_plugin_class_init(OpenautoflutterPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = openautoflutter_plugin_dispose;
}

static void openautoflutter_plugin_init(OpenautoflutterPlugin* self) {
  self->video_texture = nullptr;
  self->texture_id = 0;
  self->transport = std::make_unique<OatTransport>();
  self->decoder = std::make_shared<H264Decoder>();
  self->frame_state = std::make_shared<VideoFrameState>();
  self->texture_registrar = nullptr;
  self->frame_timer_id = 0;

  // Start as Side B (joiner) with explicit 5s wait and 1000us poll.
  g_message("OAT: starting transport as Side B (wait=5000ms poll=1000us)");
  if (!self->transport->startAsB(5000, 1000)) {
    g_warning("OAT: startAsB failed");
  } else {
    g_message("OAT: transport started (side=%d, running=%d)", static_cast<int>(self->transport->side()), self->transport->isRunning() ? 1 : 0);
    self->message_logger = std::make_unique<OatMessageLogger>(*self->transport);
    self->message_logger->install();

    auto* transport_ptr = self->transport.get();
    self->transport->setStatusCallback([transport_ptr]() {
      if (!transport_ptr) return;
      const char* side = "Unknown";
      switch (transport_ptr->side()) {
        case OatTransport::Side::A: side = "A"; break;
        case OatTransport::Side::B: side = "B"; break;
        default: break;
      }
      std::cout << "[OAT][Status] running=" << (transport_ptr->isRunning() ? 1 : 0)
                << " side=" << side
                << " sent=" << transport_ptr->sentCount()
                << " dropped=" << transport_ptr->dropCount()
                << std::endl;
    }, 5);
  }
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  OpenautoflutterPlugin* plugin = OPENAUTOFLUTTER_PLUGIN(user_data);
  openautoflutter_plugin_handle_method_call(plugin, method_call);
}

// Periodically pump decoded frames from AVConsumer into the Flutter texture.
static gboolean pump_video_frame_cb(gpointer user_data) {
  OpenautoflutterPlugin* self = OPENAUTOFLUTTER_PLUGIN(user_data);
  if (!self || !self->video_texture || !self->texture_registrar) {
    return TRUE; // keep the timer; environment not ready yet
  }

  std::vector<uint8_t> frame;
  int w = 0, h = 0;
  int64_t recv_us = 0;
  int64_t dec_us = 0;
  if (self->frame_state && self->frame_state->take_latest(frame, w, h, recv_us, dec_us)) {
    const gsize need = static_cast<gsize>(w) * static_cast<gsize>(h) * 3u / 2u;
    if (frame.size() >= need) {
      oa_video_texture_set_yuv420p_frame(self->video_texture,
                                         reinterpret_cast<const guint8*>(frame.data()),
                                         static_cast<gsize>(frame.size()),
                                         w,
                                         h);
      oa_video_texture_mark_frame_available(self->video_texture, self->texture_registrar);

      const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      const double decode_ms = dec_us > 0 && recv_us > 0 ? (dec_us - recv_us) / 1000.0 : -1.0;
      const double upload_ms = dec_us > 0 ? (now_us - dec_us) / 1000.0 : -1.0;
      const double total_ms = recv_us > 0 ? (now_us - recv_us) / 1000.0 : -1.0;

      static int log_every = 60;
      static int log_count = 0;
      if ((log_count++ % log_every) == 0) {
        std::cout << "[Timing] decode_ms=" << decode_ms
                  << " upload_ms=" << upload_ms
                  << " total_ms=" << total_ms
                  << " size=" << w << "x" << h << std::endl;
      }
    }
  }
  return TRUE; // continue calling
}

void openautoflutter_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  OpenautoflutterPlugin* plugin = OPENAUTOFLUTTER_PLUGIN(
      g_object_new(openautoflutter_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "openautoflutter",
                            FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  // Register the GL texture so Flutter can render it via a Texture widget.
  FlTextureRegistrar* texture_registrar =
      fl_plugin_registrar_get_texture_registrar(registrar);
  plugin->video_texture = oa_video_texture_new(1, 1);
  plugin->texture_id = oa_video_texture_register(plugin->video_texture, texture_registrar);
  plugin->texture_registrar = texture_registrar;

  // Start a 60 FPS timer to feed frames to Flutter when available.
  plugin->frame_timer_id = g_timeout_add(16, pump_video_frame_cb, plugin);

  g_object_unref(plugin);
}
