#pragma once
#include <memory>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <glib.h>
#include "transport.hpp"
#include "wire.hpp"

class OatTransport {
public:
    using NativeTransport = buzz::autoapp::Transport::Transport;
    using Side = NativeTransport::Side;
    using EnvelopeHandler = std::function<void(uint64_t envelope_ts, const void* data, std::size_t size)>;
    OatTransport();
    ~OatTransport();

    bool startAsB(int wait_ms = 5000, int poll_us = 1000);
    void stop();
    bool isRunning() const;
    Side side() const;
    uint64_t sentCount() const;
    uint64_t dropCount() const;
    void send(buzz::wire::MsgType type, uint64_t envelope_ts, const void* data, std::size_t size);

    void setHandler(EnvelopeHandler handler);
    void addTypeHandler(buzz::wire::MsgType type, EnvelopeHandler handler);
    void setStatusCallback(std::function<void()> callback, guint interval_seconds = 5);
    void clearStatusCallback();

    NativeTransport* raw();

private:
    std::unique_ptr<NativeTransport> transport_;
    EnvelopeHandler handler_;
    guint status_timer_id_ = 0;
    std::function<void()> status_callback_;
};

class OatMessageLogger {
public:
    explicit OatMessageLogger(OatTransport& transport);
    void install();

private:
    using MemberHandler = void (OatMessageLogger::*)(uint64_t envelope_ts, const void* data, std::size_t size);

    void registerHandler(buzz::wire::MsgType type, MemberHandler fn);
    void logPayload(const char* label, uint64_t envelope_ts, const void* data, std::size_t size);

    void handleVideo(uint64_t envelope_ts, const void* data, std::size_t size);
    void handleMediaAudio(uint64_t envelope_ts, const void* data, std::size_t size);
    void handleTouch(uint64_t envelope_ts, const void* data, std::size_t size);
    void handleControl(uint64_t envelope_ts, const void* data, std::size_t size);
    void handleGuidanceAudio(uint64_t envelope_ts, const void* data, std::size_t size);
    void handleSystemAudio(uint64_t envelope_ts, const void* data, std::size_t size);
    void handleData(uint64_t envelope_ts, const void* data, std::size_t size);
    void handleHeartbeat(uint64_t envelope_ts, const void* data, std::size_t size);

    OatTransport& transport_;
};
