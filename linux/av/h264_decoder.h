// Simple H.264 -> YUV420P (I420) decoder using libavcodec/libswscale.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class H264Decoder {
public:
	H264Decoder();
	~H264Decoder();

	// Decode to planar YUV420P (I420) and return data packed as [Y][U][V].
	// out_yuv size will be width*height*3/2 on success.
	bool decode_to_yuv420p(const uint8_t* data,
						   size_t size,
						   std::vector<uint8_t>& out_yuv,
						   int& out_width,
						   int& out_height);

private:
	struct Impl;
	Impl* impl_;
};
