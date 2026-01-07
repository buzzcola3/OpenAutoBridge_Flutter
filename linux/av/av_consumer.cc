// Simple AV consumer that attaches to shared memory buffers and logs arrivals.

#include "av_consumer.h"
#include "../common/SharedMemoryConsumer.hpp"

#include <iostream>
#include <memory>
#include <thread>
#include <cstdint>
#include <cstring>
#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>

#include "h264_decoder.h"

struct AVConsumer::Impl {
	std::unique_ptr<SharedMemoryConsumer> videoConsumer;
	std::unique_ptr<SharedMemoryConsumer> audioConsumer;
	std::thread videoThread;
	std::thread audioThread;

	// Decoder and last-decoded YUV420P frame
	H264Decoder decoder;
	std::vector<uint8_t> lastYuv420p;
	int lastW = 0;
	int lastH = 0;

	std::mutex frameMutex;
	bool newFrameAvailable = false;

	static std::string hex_head(const unsigned char* data, size_t size, size_t max_bytes = 32) {
		std::ostringstream oss;
		const size_t n = std::min(size, max_bytes);
		for (size_t i = 0; i < n; ++i) {
			if (i) oss << ' ';
			oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
		}
		if (size > max_bytes) oss << " ...";
		return oss.str();
	}

	void start() {
		const std::string videoShm = "/openauto_video_shm";
		const std::string videoSem = "/openauto_video_shm_sem";
		const size_t videoSize = 1920 * 1080 * 3;   

		const std::string audioShm = "/openauto_audio_shm";
		const std::string audioSem = "/openauto_audio_shm_sem";
		const size_t audioSize = 8192+12;

		static std::atomic<int> video_pkt_counter{0};
		videoConsumer = std::make_unique<SharedMemoryConsumer>(
			videoShm, videoSem, videoSize,
			[this](const unsigned char* buffer, size_t size) {
				// Expect header: uint64_t timestamp + uint32_t payload_size, followed by H.264 payload
				const size_t header = sizeof(uint64_t) + sizeof(uint32_t);
				if (size < header) {
					std::cout << "[AVConsumer] Video buffer too small: " << size << std::endl;
					return;
				}
				uint64_t ts = 0; uint32_t payload = 0;
				std::memcpy(&ts, buffer, sizeof(uint64_t));
				std::memcpy(&payload, buffer + sizeof(uint64_t), sizeof(uint32_t));
				const uint8_t* h264 = buffer + header;
				const size_t h264_size = size - header;
				int pkt_id = ++video_pkt_counter;
				if (pkt_id <= 10) {
					std::cout << "[AVConsumer] pkt=" << pkt_id << " ts=" << ts << " size=" << size
						<< " payload=" << payload << " h264=" << h264_size
						<< " head=" << hex_head(h264, h264_size, 32) << std::endl;
				}

				// Decode to YUV420P and store
				int w=0,h=0; std::vector<uint8_t> yuv;
				if (decoder.decode_to_yuv420p(h264, h264_size, yuv, w, h)) {
					std::lock_guard<std::mutex> lk(frameMutex);
					lastW = w;
					lastH = h;
					lastYuv420p = std::move(yuv);
					newFrameAvailable = true;
					//std::cout << "[AVConsumer] Decoded frame " << w << "x" << h << " (YUV420P)\n";
				}
			},
			10);

				audioConsumer = std::make_unique<SharedMemoryConsumer>(
			audioShm, audioSem, audioSize,
			[](const unsigned char* buffer, size_t size) {
				// Expect header: uint64_t timestamp + uint32_t payload_size
				if (size < (sizeof(uint64_t) + sizeof(uint32_t))) {
					std::cout << "[AVConsumer] Audio buffer too small: " << size << std::endl;
					return;
				}
				uint64_t ts = 0;
				uint32_t payload = 0;
				std::memcpy(&ts, buffer, sizeof(uint64_t));
				std::memcpy(&payload, buffer + sizeof(uint64_t), sizeof(uint32_t));
					std::cout << "[OAT][Audio] timestamp=" << ts << std::endl;
			},
			10);

		videoThread = std::thread([this]() { videoConsumer->run(); });
		audioThread = std::thread([this]() { audioConsumer->run(); });
	}

	void join() {
		if (videoThread.joinable()) videoThread.join();
		if (audioThread.joinable()) audioThread.join();
	}
};

AVConsumer::AVConsumer() : impl_(new Impl()) {}
AVConsumer::~AVConsumer() { delete impl_; }
void AVConsumer::start() { impl_->start(); }
void AVConsumer::join() { impl_->join(); }

bool AVConsumer::get_last_yuv420p(const uint8_t*& data, int& width, int& height, size_t& size_bytes) const {
	std::lock_guard<std::mutex> lk(impl_->frameMutex);
	if (impl_->lastYuv420p.empty() || impl_->lastW <= 0 || impl_->lastH <= 0) return false;
	data = impl_->lastYuv420p.data();
	width = impl_->lastW;
	height = impl_->lastH;
	size_bytes = impl_->lastYuv420p.size();
	return true;
}

bool AVConsumer::is_new_frame_available() const {
	std::lock_guard<std::mutex> lk(impl_->frameMutex);
	return impl_->newFrameAvailable;
}

void AVConsumer::mark_frame_consumed() {
	std::lock_guard<std::mutex> lk(impl_->frameMutex);
	impl_->newFrameAvailable = false;
}

extern "C" void av_consumer_run() {
	AVConsumer consumer;
	consumer.start();
	consumer.join();
}

