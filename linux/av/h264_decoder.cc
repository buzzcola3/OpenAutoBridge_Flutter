// H.264 -> YUV420P decoder using V4L2 stateful M2M hardware codec.

#include "h264_decoder.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <queue>
#include <stdexcept>

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */

static int xioctl(int fd, unsigned long req, void* arg) {
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}

static std::string fourcc_str(uint32_t f) {
	char s[5] = {char(f), char(f >> 8), char(f >> 16), char(f >> 24), 0};
	return s;
}

/* ------------------------------------------------------------------ */

struct H264Decoder::Impl {
	/* tunables */
	static constexpr int    OUT_BUFS     = 4;
	static constexpr int    CAP_BUFS     = 6;
	static constexpr size_t OUT_PLANE_SZ = 1u << 20; /* 1 MiB per compressed buf */

	/* mmap bookkeeping */
	struct Plane { void* ptr = MAP_FAILED; size_t len = 0; };
	struct Buf   { Plane p[VIDEO_MAX_PLANES]; };

	int fd = -1;

	Buf  out[OUT_BUFS];
	Buf  cap[CAP_BUFS];
	int  n_out = 0, n_cap = 0;
	std::queue<int> out_free;          /* indices of free OUTPUT buffers */

	/* decoded format */
	int      vis_w = 0, vis_h = 0;    /* visible (cropped) dims */
	int      cod_w = 0, cod_h = 0;    /* coded (possibly padded) dims */
	uint32_t cap_fmt = 0;
	int      cap_npl = 0;
	uint32_t cap_stride[VIDEO_MAX_PLANES] = {};

	bool out_on = false, cap_on = false, cap_ready = false;

	/* ---- lifecycle ------------------------------------------------ */

	Impl() {
		fd = open_device();
		if (fd < 0)
			throw std::runtime_error("[H264Dec] no V4L2 M2M H264 device found");
		init_output();
	}

	~Impl() {
		stream_off(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,  out_on);
		stream_off(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, cap_on);
		unmap(out, n_out);
		unmap(cap, n_cap);
		if (fd >= 0) close(fd);
	}

	/* ---- device discovery ----------------------------------------- */

	static int open_device() {
		DIR* d = opendir("/dev");
		if (!d) return -1;
		int found = -1;
		while (auto* e = readdir(d)) {
			if (strncmp(e->d_name, "video", 5) != 0) continue;
			char path[64];
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			int vfd = open(path, O_RDWR | O_NONBLOCK);
			if (vfd < 0) continue;

			v4l2_capability c{};
			if (xioctl(vfd, VIDIOC_QUERYCAP, &c) < 0 ||
				!(c.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
				close(vfd); continue;
			}

			v4l2_fmtdesc fm{};
			fm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			bool ok = false;
			while (xioctl(vfd, VIDIOC_ENUM_FMT, &fm) == 0) {
				if (fm.pixelformat == V4L2_PIX_FMT_H264) { ok = true; break; }
				fm.index++;
			}
			if (ok) {
				std::cout << "[H264Dec] V4L2 M2M device: " << path << "\n";
				found = vfd;
				break;
			}
			close(vfd);
		}
		closedir(d);
		return found;
	}

	/* ---- OUTPUT (compressed) -------------------------------------- */

	void init_output() {
		v4l2_format fmt{};
		fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		fmt.fmt.pix_mp.pixelformat          = V4L2_PIX_FMT_H264;
		fmt.fmt.pix_mp.num_planes           = 1;
		fmt.fmt.pix_mp.plane_fmt[0].sizeimage = OUT_PLANE_SZ;
		if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
			throw std::runtime_error("[H264Dec] S_FMT(OUTPUT) failed");

		/* subscribe to resolution-change events */
		v4l2_event_subscription sub{};
		sub.type = V4L2_EVENT_SOURCE_CHANGE;
		xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

		n_out = reqbufs(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, OUT_BUFS);
		for (int i = 0; i < n_out; ++i) {
			mmap_buf(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, i, 1, out[i]);
			out_free.push(i);
		}
		stream_on(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, out_on);
	}

	/* ---- CAPTURE (decoded) ---------------------------------------- */

	bool init_capture() {
		v4l2_format fmt{};
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) return false;
		if (fmt.fmt.pix_mp.width == 0) return false;

		cod_w   = fmt.fmt.pix_mp.width;
		cod_h   = fmt.fmt.pix_mp.height;
		cap_fmt = fmt.fmt.pix_mp.pixelformat;
		cap_npl = fmt.fmt.pix_mp.num_planes;
		for (int i = 0; i < cap_npl; ++i)
			cap_stride[i] = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;

		/* visible rectangle (may differ from coded size) */
		v4l2_selection sel{};
		sel.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		sel.target = V4L2_SEL_TGT_COMPOSE;
		if (xioctl(fd, VIDIOC_G_SELECTION, &sel) == 0 &&
			sel.r.width > 0 && sel.r.height > 0) {
			vis_w = sel.r.width;
			vis_h = sel.r.height;
		} else {
			vis_w = cod_w;
			vis_h = cod_h;
		}

		std::cout << "[H264Dec] capture " << vis_w << "x" << vis_h
				  << " (coded " << cod_w << "x" << cod_h << ") "
				  << fourcc_str(cap_fmt) << " planes=" << cap_npl << "\n";

		n_cap = reqbufs(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, CAP_BUFS);
		for (int i = 0; i < n_cap; ++i) {
			mmap_buf(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, cap_npl, cap[i]);
			queue_cap(i);
		}
		stream_on(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, cap_on);
		cap_ready = true;
		return true;
	}

	void reset_capture() {
		stream_off(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, cap_on);
		unmap(cap, n_cap);
		reqbufs(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
		n_cap = 0;
		cap_ready = false;
		init_capture();
	}

	/* ---- decode one packet ---------------------------------------- */

	bool decode(const uint8_t* data, size_t sz,
				std::vector<uint8_t>& yuv, int& w, int& h) {
		if (fd < 0) return false;

		/* reclaim a spent output buffer if none free */
		if (out_free.empty()) {
			v4l2_buffer b{}; v4l2_plane p[1]{};
			b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.length = 1; b.m.planes = p;
			if (xioctl(fd, VIDIOC_DQBUF, &b) == 0)
				out_free.push(b.index);
			else
				return false;
		}

		/* queue compressed data */
		int idx = out_free.front(); out_free.pop();
		size_t n = std::min(sz, out[idx].p[0].len);
		memcpy(out[idx].p[0].ptr, data, n);
		{
			v4l2_buffer b{}; v4l2_plane p[1]{};
			b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.index = idx; b.length = 1;
			p[0].bytesused = n;
			p[0].length    = out[idx].p[0].len;
			b.m.planes = p;
			if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
				out_free.push(idx);
				return false;
			}
		}

		drain_events();

		/* first-time capture setup (driver needs SPS/PPS first) */
		if (!cap_ready) {
			if (!init_capture()) return false;
		}

		/* poll for a decoded frame */
		pollfd pf{};
		pf.fd = fd; pf.events = POLLIN | POLLPRI;
		if (poll(&pf, 1, 50) <= 0) return false;

		drain_events();

		/* dequeue decoded frame */
		v4l2_buffer cb{}; v4l2_plane cp[VIDEO_MAX_PLANES]{};
		cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		cb.memory = V4L2_MEMORY_MMAP;
		cb.length = cap_npl; cb.m.planes = cp;
		if (xioctl(fd, VIDIOC_DQBUF, &cb) < 0) return false;

		w = vis_w; h = vis_h;
		extract_i420(cb.index, yuv, w, h);
		queue_cap(cb.index);
		return true;
	}

	/* ---- YUV extraction (NV12 / NV12M / YUV420M / I420) ---------- */

	void extract_i420(int i, std::vector<uint8_t>& dst, int w, int h) {
		const int y_sz  = w * h;
		const int uv_w  = (w + 1) / 2;
		const int uv_h  = (h + 1) / 2;
		const int uv_sz = uv_w * uv_h;
		dst.resize(y_sz + 2 * uv_sz);

		uint8_t* dy = dst.data();
		uint8_t* du = dy + y_sz;
		uint8_t* dv = du + uv_sz;

		switch (cap_fmt) {
		case V4L2_PIX_FMT_YUV420M:
			copy_rows(dy, cap[i].p[0].ptr, w,    h,    cap_stride[0]);
			copy_rows(du, cap[i].p[1].ptr, uv_w, uv_h, cap_stride[1]);
			copy_rows(dv, cap[i].p[2].ptr, uv_w, uv_h, cap_stride[2]);
			break;

		case V4L2_PIX_FMT_NV12M:
			copy_rows(dy, cap[i].p[0].ptr, w, h, cap_stride[0]);
			deinterleave_uv(du, dv, cap[i].p[1].ptr, uv_w, uv_h, cap_stride[1]);
			break;

		case V4L2_PIX_FMT_NV12: {
			auto* base = static_cast<const uint8_t*>(cap[i].p[0].ptr);
			copy_rows(dy, base, w, h, cap_stride[0]);
			deinterleave_uv(du, dv, base + cap_stride[0] * cod_h,
							uv_w, uv_h, cap_stride[0]);
			break;
		}

		default: {
			/* assume contiguous I420 (V4L2_PIX_FMT_YUV420) */
			auto* base = static_cast<const uint8_t*>(cap[i].p[0].ptr);
			copy_rows(dy, base, w, h, cap_stride[0]);
			int us = (cap_stride[0] + 1) / 2;
			const uint8_t* usrc = base + cap_stride[0] * cod_h;
			copy_rows(du, usrc, uv_w, uv_h, us);
			copy_rows(dv, usrc + us * ((cod_h + 1) / 2), uv_w, uv_h, us);
			break;
		}
		}
	}

	static void copy_rows(uint8_t* dst, const void* src, int w, int h, uint32_t stride) {
		auto* s = static_cast<const uint8_t*>(src);
		if (static_cast<uint32_t>(w) == stride) {
			memcpy(dst, s, static_cast<size_t>(w) * h);
		} else {
			for (int r = 0; r < h; ++r)
				memcpy(dst + r * w, s + r * stride, w);
		}
	}

	static void deinterleave_uv(uint8_t* u, uint8_t* v,
								const void* src, int w, int h, uint32_t stride) {
		auto* s = static_cast<const uint8_t*>(src);
		for (int r = 0; r < h; ++r) {
			const auto* row = s + r * stride;
			for (int c = 0; c < w; ++c) {
				u[r * w + c] = row[c * 2];
				v[r * w + c] = row[c * 2 + 1];
			}
		}
	}

	/* ---- V4L2 helpers --------------------------------------------- */

	int reqbufs(uint32_t type, int count) {
		v4l2_requestbuffers r{};
		r.count = count; r.type = type; r.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_REQBUFS, &r) < 0) return 0;
		return r.count;
	}

	void mmap_buf(uint32_t type, int index, int nplanes, Buf& dst) {
		v4l2_buffer b{}; v4l2_plane p[VIDEO_MAX_PLANES]{};
		b.type = type; b.memory = V4L2_MEMORY_MMAP;
		b.index = index; b.length = nplanes; b.m.planes = p;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
			throw std::runtime_error("[H264Dec] QUERYBUF failed");
		for (int i = 0; i < nplanes; ++i) {
			dst.p[i].len = p[i].length;
			dst.p[i].ptr = ::mmap(nullptr, p[i].length,
								  PROT_READ | PROT_WRITE, MAP_SHARED,
								  fd, p[i].m.mem_offset);
			if (dst.p[i].ptr == MAP_FAILED)
				throw std::runtime_error("[H264Dec] mmap failed");
		}
	}

	void queue_cap(int index) {
		v4l2_buffer b{}; v4l2_plane p[VIDEO_MAX_PLANES]{};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = index; b.length = cap_npl; b.m.planes = p;
		xioctl(fd, VIDIOC_QBUF, &b);
	}

	void stream_on(uint32_t type, bool& flag) {
		if (!flag && xioctl(fd, VIDIOC_STREAMON, &type) == 0) flag = true;
	}

	void stream_off(uint32_t type, bool& flag) {
		if (flag) { xioctl(fd, VIDIOC_STREAMOFF, &type); flag = false; }
	}

	void unmap(Buf* bufs, int count) {
		for (int i = 0; i < count; ++i)
			for (auto& pl : bufs[i].p)
				if (pl.ptr != MAP_FAILED && pl.ptr) {
					munmap(pl.ptr, pl.len);
					pl.ptr = MAP_FAILED;
				}
	}

	void drain_events() {
		v4l2_event ev{};
		while (xioctl(fd, VIDIOC_DQEVENT, &ev) == 0)
			if (ev.type == V4L2_EVENT_SOURCE_CHANGE) reset_capture();
	}
};

/* ------------------------------------------------------------------ */

H264Decoder::H264Decoder()  : impl_(new Impl()) {}
H264Decoder::~H264Decoder() { delete impl_; }

bool H264Decoder::decode_to_yuv420p(const uint8_t* data, size_t size,
									std::vector<uint8_t>& out_yuv,
									int& out_width, int& out_height) {
	return impl_->decode(data, size, out_yuv, out_width, out_height);
}
