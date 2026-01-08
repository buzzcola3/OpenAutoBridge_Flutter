#include "include/openautoflutter/openautoflutter_plugin.h"
#include "av/oa_video_texture.h"
#include "transport.hpp"
#include "wire.hpp"
#include "oat_message_handlers.h"
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
using NativeTransport = buzz::autoapp::Transport::Transport;

#include "openautoflutter_plugin_private.h"

namespace {
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
  std::unique_ptr<NativeTransport> transport; // OpenAutoTransport receiver
  std::unique_ptr<OatMessageHandlers> handlers;
  FlTextureRegistrar* texture_registrar; // to mark frames available
  bool handlers_installed;
};

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
  if (self->handlers) {
    self->handlers.reset();
  }
  if (self->transport) {
    self->transport->stop();
    self->transport.reset();
  }
  if (self->video_texture != nullptr) {
    g_clear_object(&self->video_texture);
  }
  G_OBJECT_CLASS(openautoflutter_plugin_parent_class)->dispose(object);
}

static void openautoflutter_plugin_class_init(OpenautoflutterPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = openautoflutter_plugin_dispose;
}

static void openautoflutter_plugin_init(OpenautoflutterPlugin* self) {
  self->video_texture = nullptr;
  self->texture_id = 0;
  self->transport = std::make_unique<NativeTransport>();
  self->texture_registrar = nullptr;
  self->handlers_installed = false;

  // Start as Side B (joiner) with explicit 5s wait and 1000us poll.
  g_message("OAT: starting transport as Side B (wait=5000ms poll=1000us)");
  const auto wait_duration = std::chrono::milliseconds{5000};
  const auto poll_interval = std::chrono::microseconds{1000};
  if (!self->transport->startAsB(wait_duration, poll_interval)) {
    g_warning("OAT: startAsB failed");
  } else {
    g_message("OAT: transport started (side=%d, running=%d)", static_cast<int>(self->transport->side()), self->transport->isRunning() ? 1 : 0);
  }
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  OpenautoflutterPlugin* plugin = OPENAUTOFLUTTER_PLUGIN(user_data);
  openautoflutter_plugin_handle_method_call(plugin, method_call);
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

  if (plugin->transport && !plugin->handlers_installed) {
    plugin->handlers = std::make_unique<OatMessageHandlers>(*plugin->transport, plugin->video_texture, plugin->texture_registrar);
    plugin->handlers->install();
    plugin->handlers_installed = true;
  }

  g_object_unref(plugin);
}
