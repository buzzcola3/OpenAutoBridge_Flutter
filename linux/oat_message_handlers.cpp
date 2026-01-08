#include "oat_message_handlers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

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
