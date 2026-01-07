#include "oat_transport.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>

OatTransport::OatTransport() {
    transport_ = std::make_unique<NativeTransport>();
}

OatTransport::~OatTransport() {
    clearStatusCallback();
    stop();
}

bool OatTransport::startAsB(int wait_ms, int poll_us) {
    return transport_ && transport_->startAsB(std::chrono::milliseconds{wait_ms}, std::chrono::microseconds{poll_us});
}

void OatTransport::stop() {
    if (transport_) transport_->stop();
}

bool OatTransport::isRunning() const {
    return transport_ && transport_->isRunning();
}

OatTransport::Side OatTransport::side() const {
    return transport_ ? transport_->side() : Side::Unknown;
}

uint64_t OatTransport::sentCount() const {
    return transport_ ? transport_->sentCount() : 0;
}

uint64_t OatTransport::dropCount() const {
    return transport_ ? transport_->dropCount() : 0;
}

void OatTransport::send(buzz::wire::MsgType type, uint64_t envelope_ts, const void* data, std::size_t size) {
    if (transport_) {
        transport_->send(type, envelope_ts, data, size);
    }
}

void OatTransport::setHandler(EnvelopeHandler handler) {
    handler_ = std::move(handler);
    if (transport_) {
        transport_->setHandler(handler_);
    }
}

void OatTransport::addTypeHandler(buzz::wire::MsgType type, EnvelopeHandler handler) {
    if (!transport_) return;
    transport_->addTypeHandler(type, std::move(handler));
}

void OatTransport::setStatusCallback(std::function<void()> callback, guint interval_seconds) {
    clearStatusCallback();
    status_callback_ = std::move(callback);
    status_timer_id_ = g_timeout_add_seconds(interval_seconds, [](gpointer user_data) -> gboolean {
        auto* self = static_cast<OatTransport*>(user_data);
        if (self && self->status_callback_) self->status_callback_();
        return TRUE;
    }, this);
}

void OatTransport::clearStatusCallback() {
    if (status_timer_id_) {
        g_source_remove(status_timer_id_);
        status_timer_id_ = 0;
    }
    status_callback_ = nullptr;
}

OatTransport::NativeTransport* OatTransport::raw() {
    return transport_.get();
}

namespace {
std::string hex_head(const uint8_t* data, std::size_t size, std::size_t max_bytes = 32) {
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

} // namespace

OatMessageLogger::OatMessageLogger(OatTransport& transport)
    : transport_(transport) {}

void OatMessageLogger::install() {
    struct Entry {
        buzz::wire::MsgType type;
        MemberHandler fn;
    };

    static constexpr std::array<Entry, 8> kEntries = {
        Entry{buzz::wire::MsgType::VIDEO, &OatMessageLogger::handleVideo},
        Entry{buzz::wire::MsgType::MEDIA_AUDIO, &OatMessageLogger::handleMediaAudio},
        Entry{buzz::wire::MsgType::TOUCH, &OatMessageLogger::handleTouch},
        Entry{buzz::wire::MsgType::CONTROL, &OatMessageLogger::handleControl},
        Entry{buzz::wire::MsgType::GUIDANCE_AUDIO, &OatMessageLogger::handleGuidanceAudio},
        Entry{buzz::wire::MsgType::SYSTEM_AUDIO, &OatMessageLogger::handleSystemAudio},
        Entry{buzz::wire::MsgType::DATA, &OatMessageLogger::handleData},
        Entry{buzz::wire::MsgType::HEARTBEAT, &OatMessageLogger::handleHeartbeat}
    };

    for (const auto& entry : kEntries) {
        registerHandler(entry.type, entry.fn);
    }
}

void OatMessageLogger::registerHandler(buzz::wire::MsgType type, MemberHandler fn) {
    transport_.addTypeHandler(type, [this, fn](uint64_t envelope_ts, const void* data, std::size_t size) {
        (this->*fn)(envelope_ts, data, size);
    });
}

void OatMessageLogger::logPayload(const char* label, uint64_t envelope_ts, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::cout << "[OAT][" << label << "] ts=" << envelope_ts
              << " size=" << size
              << " head=" << hex_head(bytes, size, 32)
              << std::endl;
}

void OatMessageLogger::handleVideo(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("VIDEO", envelope_ts, data, size);
}

void OatMessageLogger::handleMediaAudio(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("MEDIA_AUDIO", envelope_ts, data, size);
}

void OatMessageLogger::handleTouch(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("TOUCH", envelope_ts, data, size);
}

void OatMessageLogger::handleControl(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("CONTROL", envelope_ts, data, size);
}

void OatMessageLogger::handleGuidanceAudio(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("GUIDANCE_AUDIO", envelope_ts, data, size);
}

void OatMessageLogger::handleSystemAudio(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("SYSTEM_AUDIO", envelope_ts, data, size);
}

void OatMessageLogger::handleData(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("DATA", envelope_ts, data, size);
}

void OatMessageLogger::handleHeartbeat(uint64_t envelope_ts, const void* data, std::size_t size) {
    logPayload("HEARTBEAT", envelope_ts, data, size);
}
