#include "h264_decoder.h"

#include <memory>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct H264Decoder::Impl {
	const AVCodec* codec = nullptr;
	AVCodecContext* ctx = nullptr;
	AVFrame* frame = nullptr;
	AVPacket* pkt = nullptr;
	SwsContext* sws = nullptr;

	Impl() {
		codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) throw std::runtime_error("H264 codec not found");
		ctx = avcodec_alloc_context3(codec);
		if (!ctx) throw std::runtime_error("Failed to alloc codec context");
		if (avcodec_open2(ctx, codec, nullptr) < 0)
			throw std::runtime_error("Failed to open codec");
		frame = av_frame_alloc();
		pkt = av_packet_alloc();
		if (!frame || !pkt) throw std::runtime_error("Failed to alloc frame/pkt");
	}

	~Impl() {
		if (sws) sws_freeContext(sws);
		if (pkt) av_packet_free(&pkt);
		if (frame) av_frame_free(&frame);
		if (ctx) avcodec_free_context(&ctx);
	}
};

H264Decoder::H264Decoder() : impl_(new Impl()) {}
H264Decoder::~H264Decoder() { delete impl_; }

bool H264Decoder::decode_to_yuv420p(const uint8_t* data,
									size_t size,
									std::vector<uint8_t>& out_yuv,
									int& out_width,
									int& out_height) {
	auto* impl = impl_;
	av_packet_unref(impl->pkt);
	impl->pkt->data = const_cast<uint8_t*>(data);
	impl->pkt->size = static_cast<int>(size);

	int ret = avcodec_send_packet(impl->ctx, impl->pkt);
	if (ret < 0) return false;

	while (true) {
		ret = avcodec_receive_frame(impl->ctx, impl->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
		if (ret < 0) return false;

		AVFrame* f = impl->frame;
		out_width = f->width;
		out_height = f->height;
		if (out_width <= 0 || out_height <= 0) return false;

		SwsContext* sws = sws_getContext(
			f->width, f->height, static_cast<AVPixelFormat>(f->format),
			f->width, f->height, AV_PIX_FMT_YUV420P,
			SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws) return false;

		const int y_size = out_width * out_height;
		const int uv_w = (out_width + 1) / 2;
		const int uv_h = (out_height + 1) / 2;
		const int uv_size = uv_w * uv_h;
		out_yuv.resize(y_size + uv_size * 2);

		uint8_t* dst_data[4] = {
			out_yuv.data(),                         // Y
			out_yuv.data() + y_size,                // U
			out_yuv.data() + y_size + uv_size,      // V
			nullptr
		};
		int dst_linesize[4] = {
			out_width,
			uv_w,
			uv_w,
			0
		};

		sws_scale(sws, f->data, f->linesize, 0, out_height, dst_data, dst_linesize);
		sws_freeContext(sws);
		return true;
	}
	return false;
}