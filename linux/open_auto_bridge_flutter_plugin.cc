#include "include/open_auto_bridge_flutter/open_auto_bridge_flutter_plugin.h"
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

using NativeTransport = buzz::autoapp::Transport::Transport;

#include "open_auto_bridge_flutter_plugin_private.h"



#define OPEN_AUTO_BRIDGE_FLUTTER_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), open_auto_bridge_flutter_plugin_get_type(), \
                              OpenAutoBridgeFlutterPlugin))

struct _OpenAutoBridgeFlutterPlugin {
  GObject parent_instance;
  OAVideoTexture* video_texture;
  int64_t texture_id;
  std::unique_ptr<NativeTransport> transport; // OpenAutoTransport receiver
  std::unique_ptr<OatMessageHandlers> handlers;
  FlTextureRegistrar* texture_registrar; // to mark frames available
  FlMethodChannel* method_channel; // kept alive for native→Dart calls
  bool handlers_installed;
  std::unique_ptr<DropBuffer> drop_buffer; // shared state between decoder & texture
};

G_DEFINE_TYPE(OpenAutoBridgeFlutterPlugin, open_auto_bridge_flutter_plugin, g_object_get_type())

// Called when a method call is received from Flutter.
static void open_auto_bridge_flutter_plugin_handle_method_call(
    OpenAutoBridgeFlutterPlugin* self,
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
    std::string error;
    if (!self->handlers) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("not_ready", "Handlers not initialized", nullptr));
    } else if (!self->handlers->handleTouchMethod(fl_method_call_get_args(method_call), error)) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("invalid_args", error.c_str(), nullptr));
    } else {
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    }
  } else if (strcmp(method, "sendSensorJson") == 0) {
    std::string error;
    if (!self->handlers) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("not_ready", "Handlers not initialized", nullptr));
    } else if (!self->handlers->handleSensorMethod(fl_method_call_get_args(method_call), error)) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("invalid_args", error.c_str(), nullptr));
    } else {
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    }
  } else if (strcmp(method, "sendConfigJson") == 0) {
    std::string error;
    if (!self->handlers) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("not_ready", "Handlers not initialized", nullptr));
    } else if (!self->handlers->handleConfigMethod(fl_method_call_get_args(method_call), error)) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("invalid_args", error.c_str(), nullptr));
    } else {
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
    }
  } else if (strcmp(method, "sendControlJson") == 0) {
    std::string error;
    if (!self->handlers) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("not_ready", "Handlers not initialized", nullptr));
    } else if (!self->handlers->handleControlMethod(fl_method_call_get_args(method_call), error)) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("invalid_args", error.c_str(), nullptr));
    } else {
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

static void open_auto_bridge_flutter_plugin_dispose(GObject* object) {
  OpenAutoBridgeFlutterPlugin* self = OPEN_AUTO_BRIDGE_FLUTTER_PLUGIN(object);
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
  g_clear_object(&self->method_channel);
  self->drop_buffer.reset();
  G_OBJECT_CLASS(open_auto_bridge_flutter_plugin_parent_class)->dispose(object);
}

static void open_auto_bridge_flutter_plugin_class_init(OpenAutoBridgeFlutterPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = open_auto_bridge_flutter_plugin_dispose;
}

static void open_auto_bridge_flutter_plugin_init(OpenAutoBridgeFlutterPlugin* self) {
  self->video_texture = nullptr;
  self->texture_id = 0;
  self->transport = std::make_unique<NativeTransport>();
  self->texture_registrar = nullptr;
  self->method_channel = nullptr;
  self->handlers_installed = false;
  self->drop_buffer = std::make_unique<DropBuffer>();

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
  OpenAutoBridgeFlutterPlugin* plugin = OPEN_AUTO_BRIDGE_FLUTTER_PLUGIN(user_data);
  open_auto_bridge_flutter_plugin_handle_method_call(plugin, method_call);
}

void open_auto_bridge_flutter_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  OpenAutoBridgeFlutterPlugin* plugin = OPEN_AUTO_BRIDGE_FLUTTER_PLUGIN(
      g_object_new(open_auto_bridge_flutter_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  FlMethodChannel* channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "open_auto_bridge_flutter",
                            FL_METHOD_CODEC(codec));
  plugin->method_channel = channel;
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  // Register the GL texture so Flutter can render it via a Texture widget.
  FlTextureRegistrar* texture_registrar =
      fl_plugin_registrar_get_texture_registrar(registrar);
  plugin->video_texture = oa_video_texture_new(plugin->drop_buffer.get());
  plugin->texture_id = oa_video_texture_register(plugin->video_texture, texture_registrar);
  plugin->texture_registrar = texture_registrar;

  if (plugin->transport && !plugin->handlers_installed) {
    plugin->handlers = std::make_unique<OatMessageHandlers>(*plugin->transport, plugin->video_texture, plugin->texture_registrar, plugin->drop_buffer.get(), plugin->method_channel);
    plugin->handlers->install();
    plugin->handlers_installed = true;
  }

  g_object_unref(plugin);
}
