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
} // namespace

OatMessageHandlers::OatMessageHandlers(NativeTransport& transport,
                                                                             OAVideoTexture* texture,
                                                                             FlTextureRegistrar* registrar)
        : transport_(transport),
            texture_(texture),
            registrar_(registrar),
            video_state_(std::make_shared<VideoState>()),
            installed_(false) {
    video_state_->decoder = std::make_shared<H264Decoder>();
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

void OatMessageHandlers::logPayload(const char* label, uint64_t envelope_ts, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::cout << "[OAT][" << label << "] ts=" << envelope_ts
                        << " size=" << size
                        << " head=" << hexHead(bytes, size, 32)
                        << std::endl;
}

void OatMessageHandlers::handleVideo(uint64_t envelope_ts, const void* data, std::size_t size) {
    //logPayload("VIDEO", envelope_ts, data, size);
    if (!video_state_ || !video_state_->decoder || !texture_ || !registrar_ || !data || size == 0) return;

    std::vector<uint8_t> decoded;
    int w = 0;
    int h = 0;
    if (!video_state_->decoder->decode_to_yuv420p(static_cast<const uint8_t*>(data), size, decoded, w, h)) {
        return;
    }

    const gsize need = static_cast<gsize>(w) * static_cast<gsize>(h) * 3u / 2u;
    if (decoded.size() < need || need == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(video_state_->mutex);
        video_state_->yuv.swap(decoded);
    }

    oa_video_texture_set_yuv420p_frame(texture_,
                                                                         reinterpret_cast<const guint8*>(video_state_->yuv.data()),
                                                                         static_cast<gsize>(video_state_->yuv.size()),
                                                                         w,
                                                                         h);
    oa_video_texture_mark_frame_available(texture_, registrar_);
}

void OatMessageHandlers::install() {
    if (installed_) return;
    installed_ = true;

    transport_.addTypeHandler(buzz::wire::MsgType::VIDEO,
            [this](uint64_t ts, const void* data, std::size_t size) {
                handleVideo(ts, data, size);
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
