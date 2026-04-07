#include "oat_message_handlers.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include <glib.h>

using TouchPayload = NativeTransport::TouchEventPayload;

namespace {
enum class TouchAction : uint32_t {
    DOWN = 0,
    UP = 1,
    MOVED = 2,
    POINTER_DOWN = 3,
    POINTER_UP = 4,
};

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

bool parse_touch_args(FlValue* args, TouchPayload& out, std::string& error) {
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
    out.pointerId = pid_val < 0 ? 0u : static_cast<uint32_t>(pid_val);
    out.action = static_cast<uint32_t>(action_enum);
    return true;
}

bool parse_sensor_args(FlValue* args, std::string& json, std::string& error) {
    if (!args) {
        error = "Args are required";
        return false;
    }
    if (fl_value_get_type(args) == FL_VALUE_TYPE_STRING) {
        json = fl_value_get_string(args);
        return true;
    }
    if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
        error = "Args must be a map";
        return false;
    }

    FlValue* json_value = fl_value_lookup_string(args, "json");
    if (!json_value || fl_value_get_type(json_value) != FL_VALUE_TYPE_STRING) {
        error = "Missing or invalid json";
        return false;
    }

    json = fl_value_get_string(json_value);
    return true;
}
} // namespace

OatMessageHandlers::OatMessageHandlers(NativeTransport& transport,
                                                                             OAVideoTexture* texture,
                                                                             FlTextureRegistrar* registrar,
                                                                             DropBuffer* drop_buffer,
                                                                             FlMethodChannel* channel)
        : transport_(transport),
            texture_(texture),
            registrar_(registrar),
            drop_buffer_(drop_buffer),
            channel_(channel),
            decoder_(),
            installed_(false) {
    decoder_.set_buffer(drop_buffer_);
    decoder_.set_frame_ready_callback([this]() {
        if (texture_ && registrar_) {
            oa_video_texture_mark_frame_available(texture_, registrar_);
        }
    });
    decoder_.start();

    // Create audio players for each channel
    media_audio_   = std::make_unique<OAAudioPlayer>(OAAudioPlayer::Config{48000, 2, 16});
    guidance_audio_ = std::make_unique<OAAudioPlayer>(OAAudioPlayer::Config{16000, 1, 16});
    system_audio_  = std::make_unique<OAAudioPlayer>(OAAudioPlayer::Config{16000, 1, 16});
}

std::string OatMessageHandlers::hexHead(const uint8_t* data, std::size_t size, std::size_t max_bytes) {
    if (!data || size == 0) return {};
    std::ostringstream oss;
    const std::size_t n = std::min(size, max_bytes);
    for (std::size_t i = 0; i < n; ++i) {
        if (i) oss << ' ';
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    if (size > max_bytes) oss << " ...";
    return oss.str();
}

void OatMessageHandlers::handleVideo(uint64_t envelope_ts, const void* data, std::size_t size) {
    if (!data || size == 0) return;
    decoder_.push_h264(static_cast<const uint8_t*>(data), size);
}

void OatMessageHandlers::install() {
    if (installed_) return;
    installed_ = true;

    transport_.addTypeHandler(buzz::wire::MsgType::VIDEO,
            [this](uint64_t ts, const void* data, std::size_t size) {
                handleVideo(ts, data, size);
            });

    transport_.addTypeHandler(buzz::wire::MsgType::CONFIGURATION,
            [this](uint64_t ts, const void* data, std::size_t size) {
                handleConfigResponse(ts, data, size);
            });

    transport_.addTypeHandler(buzz::wire::MsgType::CONTROL,
            [this](uint64_t ts, const void* data, std::size_t size) {
                handleControlResponse(ts, data, size);
            });

    transport_.addTypeHandler(buzz::wire::MsgType::MEDIA_AUDIO,
            [this](uint64_t ts, const void* data, std::size_t size) {
                handleAudio(buzz::wire::MsgType::MEDIA_AUDIO, ts, data, size);
            });

    transport_.addTypeHandler(buzz::wire::MsgType::GUIDANCE_AUDIO,
            [this](uint64_t ts, const void* data, std::size_t size) {
                handleAudio(buzz::wire::MsgType::GUIDANCE_AUDIO, ts, data, size);
            });

    transport_.addTypeHandler(buzz::wire::MsgType::SYSTEM_AUDIO,
            [this](uint64_t ts, const void* data, std::size_t size) {
                handleAudio(buzz::wire::MsgType::SYSTEM_AUDIO, ts, data, size);
            });
}

bool OatMessageHandlers::handleTouchMethod(FlValue* args, std::string& error) {
    TouchPayload touch_msg{};
    if (!parse_touch_args(args, touch_msg, error)) {
        return false;
    }

    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (transport_.isRunning()) {
        transport_.sendTouch(static_cast<uint64_t>(now_us), touch_msg);
    } else {
        g_warning("OAT: transport not running; dropping touch event");
    }
    return true;
}

bool OatMessageHandlers::handleSensorMethod(FlValue* args, std::string& error) {
    std::string json;
    if (!parse_sensor_args(args, json, error)) {
        return false;
    }

    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (transport_.isRunning()) {
        transport_.send(buzz::wire::MsgType::SENSOR,
                        static_cast<uint64_t>(now_us),
                        json.data(),
                        json.size());
    } else {
        g_warning("OAT: transport not running; dropping sensor event");
    }
    return true;
}

bool OatMessageHandlers::handleConfigMethod(FlValue* args, std::string& error) {
    std::string json;
    if (!parse_sensor_args(args, json, error)) {
        return false;
    }

    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (transport_.isRunning()) {
        transport_.send(buzz::wire::MsgType::CONFIGURATION,
                        static_cast<uint64_t>(now_us),
                        json.data(),
                        json.size());
    } else {
        g_warning("OAT: transport not running; dropping config event");
    }
    return true;
}

// --- Responses from core (transport thread → main thread) -----------------

namespace {
struct ChannelCbData {
    FlMethodChannel* channel;
    const char* method_name;
    std::string json;
};

gboolean channel_invoke_idle_cb(gpointer user_data) {
    auto* d = static_cast<ChannelCbData*>(user_data);
    g_autoptr(FlValue) args = fl_value_new_string(d->json.c_str());
    fl_method_channel_invoke_method(d->channel, d->method_name, args,
                                    nullptr, nullptr, nullptr);
    delete d;
    return G_SOURCE_REMOVE;
}
}  // namespace

void OatMessageHandlers::handleConfigResponse(uint64_t /*envelope_ts*/,
                                              const void* data,
                                              std::size_t size) {
    if (!channel_) return;
    auto* cb = new ChannelCbData{channel_, "onConfigReceived",
                                 std::string(static_cast<const char*>(data), size)};
    g_idle_add(channel_invoke_idle_cb, cb);
}

bool OatMessageHandlers::handleControlMethod(FlValue* args, std::string& error) {
    std::string json;
    if (!parse_sensor_args(args, json, error)) {
        return false;
    }

    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (transport_.isRunning()) {
        transport_.send(buzz::wire::MsgType::CONTROL,
                        static_cast<uint64_t>(now_us),
                        json.data(),
                        json.size());
    } else {
        g_warning("OAT: transport not running; dropping control event");
    }
    return true;
}

void OatMessageHandlers::handleControlResponse(uint64_t /*envelope_ts*/,
                                               const void* data,
                                               std::size_t size) {
    if (!channel_) return;
    auto* cb = new ChannelCbData{channel_, "onControlReceived",
                                 std::string(static_cast<const char*>(data), size)};
    g_idle_add(channel_invoke_idle_cb, cb);
}

// --- Audio handling -------------------------------------------------------

void OatMessageHandlers::handleAudio(buzz::wire::MsgType type, uint64_t ts,
                                     const void* data, std::size_t size) {
    if (!data || size == 0) return;
    switch (type) {
        case buzz::wire::MsgType::MEDIA_AUDIO:
            if (media_audio_)   media_audio_->push(data, size, ts); break;
        case buzz::wire::MsgType::GUIDANCE_AUDIO:
            if (guidance_audio_) guidance_audio_->push(data, size, ts); break;
        case buzz::wire::MsgType::SYSTEM_AUDIO:
            if (system_audio_)  system_audio_->push(data, size, ts); break;
        default: break;
    }
}

bool OatMessageHandlers::handleAudioVolumeMethod(FlValue* args, std::string& error) {
    if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
        error = "Args must be a map";
        return false;
    }

    FlValue* ch_val = fl_value_lookup_string(args, "channel");
    FlValue* vol_val = fl_value_lookup_string(args, "volume");
    if (!ch_val || fl_value_get_type(ch_val) != FL_VALUE_TYPE_STRING) {
        error = "Missing or invalid channel";
        return false;
    }
    bool ok = false;
    const int vol = static_cast<int>(get_number(vol_val, ok));
    if (!ok) {
        error = "Missing or invalid volume";
        return false;
    }

    const std::string channel = fl_value_get_string(ch_val);
    if (channel == "media" && media_audio_) {
        media_audio_->setVolume(vol);
    } else if (channel == "guidance" && guidance_audio_) {
        guidance_audio_->setVolume(vol);
    } else if (channel == "system" && system_audio_) {
        system_audio_->setVolume(vol);
    } else {
        error = "Unknown audio channel: " + channel;
        return false;
    }
    return true;
}

bool OatMessageHandlers::handleAudioDeviceMethod(FlValue* args, std::string& error) {
    if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
        error = "Args must be a map";
        return false;
    }

    FlValue* dev_val = fl_value_lookup_string(args, "device");
    if (!dev_val || fl_value_get_type(dev_val) != FL_VALUE_TYPE_STRING) {
        error = "Missing or invalid device";
        return false;
    }
    const std::string device = fl_value_get_string(dev_val);

    if (media_audio_)   media_audio_->setDevice(device);
    if (guidance_audio_) guidance_audio_->setDevice(device);
    if (system_audio_)  system_audio_->setDevice(device);
    return true;
}
