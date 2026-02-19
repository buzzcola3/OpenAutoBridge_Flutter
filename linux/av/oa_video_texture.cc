#include "oa_video_texture.h"

#include <epoxy/gl.h>
#include <flutter_linux/flutter_linux.h>
#include <atomic>
#include <iostream>
#include <string.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include <cinttypes>

// ──────────────────────────────────────────────────────────────────────────────
// Debug toggle: set to 1 for verbose buffer/GL lifecycle tracing, 0 for release
// ──────────────────────────────────────────────────────────────────────────────
#ifndef OA_TEX_DEBUG
#define OA_TEX_DEBUG 1
#endif

#if OA_TEX_DEBUG
#define DBG(fmt, ...) \
	std::cerr << "[OAVideoTexture][DBG] " << fmt << std::endl
#define DBGF(tag, fmt, ...) \
	fprintf(stderr, "[OAVideoTexture][DBG][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define DBG(fmt, ...) ((void)0)
#define DBGF(tag, fmt, ...) ((void)0)
#endif

// ──────────────────────────────────────────────────────────────────────────────
// Buffer tracker: logs every alloc, resize, read, and free of GByteArray
// buffers so we can spot overflows and use-after-free.
// ──────────────────────────────────────────────────────────────────────────────
struct BufferDebugInfo {
	const char* name;        // human-readable label ("pixels", "yuv")
	gsize      alloc_size;   // last known allocated size (GByteArray->len)
	bool       alive;        // false after g_byte_array_unref
	int        resize_count; // how many times resized
	int        read_count;   // how many times read from in populate()
	int        write_count;  // how many times written via set_*_frame()
};

static void buf_track_init(BufferDebugInfo& info, const char* name, GByteArray* buf) {
	info.name = name;
	info.alloc_size = buf ? buf->len : 0;
	info.alive = (buf != nullptr);
	info.resize_count = 0;
	info.read_count = 0;
	info.write_count = 0;
	DBGF("BUF", "%s: INIT alloc_size=%zu alive=%d ptr=%p",
		 name, (size_t)info.alloc_size, info.alive, (void*)(buf ? buf->data : nullptr));
}

static void buf_track_resize(BufferDebugInfo& info, GByteArray* buf, gsize new_size,
							 const char* caller) {
	if (!info.alive) {
		DBGF("BUF", "!!! %s: RESIZE AFTER FREE (use-after-free!) caller=%s new_size=%zu",
			 info.name, caller, (size_t)new_size);
	}
	gsize old_size = info.alloc_size;
	info.alloc_size = new_size;
	info.resize_count++;
	DBGF("BUF", "%s: RESIZE #%d  %zu -> %zu bytes  caller=%s  ptr=%p",
		 info.name, info.resize_count, (size_t)old_size, (size_t)new_size,
		 caller, (void*)(buf ? buf->data : nullptr));
}

static void buf_track_read(BufferDebugInfo& info, GByteArray* buf, gsize read_offset,
						   gsize read_len, const char* caller) {
	info.read_count++;
	if (!info.alive) {
		DBGF("BUF", "!!! %s: READ AFTER FREE (use-after-free!) caller=%s offset=%zu len=%zu",
			 info.name, caller, (size_t)read_offset, (size_t)read_len);
	}
	if (buf && (read_offset + read_len > buf->len)) {
		DBGF("BUF", "!!! %s: BUFFER OVERFLOW on read! offset=%zu + len=%zu = %zu > alloc=%u  caller=%s",
			 info.name, (size_t)read_offset, (size_t)read_len,
			 (size_t)(read_offset + read_len), buf->len, caller);
	} else {
		DBGF("BUF", "%s: READ #%d  offset=%zu len=%zu  alloc=%zu  headroom=%zu  caller=%s",
			 info.name, info.read_count,
			 (size_t)read_offset, (size_t)read_len,
			 (size_t)(buf ? buf->len : 0),
			 (size_t)(buf ? (buf->len - read_offset - read_len) : 0),
			 caller);
	}
}

static void buf_track_write(BufferDebugInfo& info, GByteArray* buf, gsize write_offset,
							gsize write_len, const char* caller) {
	info.write_count++;
	if (!info.alive) {
		DBGF("BUF", "!!! %s: WRITE AFTER FREE (use-after-free!) caller=%s offset=%zu len=%zu",
			 info.name, caller, (size_t)write_offset, (size_t)write_len);
	}
	if (buf && (write_offset + write_len > buf->len)) {
		DBGF("BUF", "!!! %s: BUFFER OVERFLOW on write! offset=%zu + len=%zu = %zu > alloc=%u  caller=%s",
			 info.name, (size_t)write_offset, (size_t)write_len,
			 (size_t)(write_offset + write_len), buf->len, caller);
	} else {
		DBGF("BUF", "%s: WRITE #%d  offset=%zu len=%zu  alloc=%zu  headroom=%zu  caller=%s",
			 info.name, info.write_count,
			 (size_t)write_offset, (size_t)write_len,
			 (size_t)(buf ? buf->len : 0),
			 (size_t)(buf ? (buf->len - write_offset - write_len) : 0),
			 caller);
	}
}

static void buf_track_free(BufferDebugInfo& info, const char* caller) {
	if (!info.alive) {
		DBGF("BUF", "!!! %s: DOUBLE FREE! caller=%s", info.name, caller);
	} else {
		DBGF("BUF", "%s: FREE  last_alloc=%zu  total_resizes=%d  total_reads=%d  total_writes=%d  caller=%s",
			 info.name, (size_t)info.alloc_size, info.resize_count,
			 info.read_count, info.write_count, caller);
	}
	info.alive = false;
	info.alloc_size = 0;
}

// ──────────────────────────────────────────────────────────────────────────────
// Instance struct
// ──────────────────────────────────────────────────────────────────────────────
struct _OAVideoTexture {
	FlTextureGL parent_instance;

	// Output GL texture
	GLuint gl_tex = 0;
	int*   width  = nullptr;
	int*   height = nullptr;

	// RGBA buffer path
	GByteArray* pixels = nullptr;

	// YUV420P packed buffer: [Y plane][U plane][V plane]
	GByteArray* yuv     = nullptr;
	gboolean    has_yuv = FALSE;

	// GL resources for YUV→RGBA conversion
	GLuint y_tex   = 0;    // Y-plane texture
	GLuint u_tex   = 0;    // U-plane texture
	GLuint v_tex   = 0;    // V-plane texture
	GLuint fbo     = 0;    // framebuffer targeting gl_tex
	GLuint program = 0;    // YUV→RGBA shader program
	GLuint vbo     = 0;    // fullscreen quad VBO

	// Shader attribute/uniform locations
	GLint loc_aPos = -1;
	GLint loc_aTex = -1;
	GLint loc_texY = -1;
	GLint loc_texU = -1;
	GLint loc_texV = -1;

	int64_t registered_id = 0;

	// Last-allocated texture dimensions (to avoid glTexImage2D orphaning)
	int rgba_tex_w = 0; // RGBA output width
	int rgba_tex_h = 0; // RGBA output height
	int y_tex_w = 0;    // Y plane width
	int y_tex_h = 0;    // Y plane height
	int u_tex_w = 0;    // U plane width
	int u_tex_h = 0;    // U plane height
	int v_tex_w = 0;    // V plane width
	int v_tex_h = 0;    // V plane height

	std::mutex mutex; // protects pixels, yuv, width, height, has_yuv

	// Debug tracking
	BufferDebugInfo dbg_pixels;
	BufferDebugInfo dbg_yuv;
	bool            gl_resources_inited = false;
};

struct _OAVideoTextureClass {
	FlTextureGLClass parent_class;
};

G_DEFINE_TYPE(OAVideoTexture, oa_video_texture, fl_texture_gl_get_type())

// ──────────────────────────────────────────────────────────────────────────────
// GL helpers
// ──────────────────────────────────────────────────────────────────────────────

static bool log_gl_errors(const char* stage) {
	bool had_error = false;
	GLenum err;
	while ((err = glGetError()) != GL_NO_ERROR) {
		had_error = true;
		std::cerr << "[OAVideoTexture][GL] error 0x" << std::hex << err
				  << std::dec << " at " << stage << std::endl;
	}
	return had_error;
}

static GLuint compile_shader(GLenum type, const char* src, const char* label) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);

	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		GLsizei len = 0;
		glGetShaderInfoLog(s, sizeof(log) - 1, &len, log);
		std::cerr << "[OAVideoTexture][GL] " << label << " compile failed: "
				  << std::string(log, (size_t)len) << std::endl;
		glDeleteShader(s);
		return 0;
	}
	DBGF("GL", "%s compiled OK (id=%u)", label, s);
	return s;
}

/// Detect whether the GL context is GLES and its GLSL version.
static void query_glsl_profile(bool& is_es, float& version) {
	is_es = false;
	version = 0.0f;
	const char* sl = reinterpret_cast<const char*>(
		glGetString(GL_SHADING_LANGUAGE_VERSION));
	if (!sl) return;
	is_es = (strstr(sl, "ES") != nullptr);
	// Skip non-digit prefix to find version number
	const char* p = sl;
	while (*p && ((*p < '0' || *p > '9') && *p != '.')) ++p;
	if (*p) version = strtof(p, nullptr);
	DBGF("GL", "GLSL profile: %s  version=%.1f  string=\"%s\"",
		 is_es ? "ES" : "Desktop", version, sl);
}

// ──────────────────────────────────────────────────────────────────────────────
// YUV→RGBA shader — single source, profile-selected at runtime
//
// Both BT.601 limited-range coefficients:
//   R = Y + 1.402  * (V - 128/255)
//   G = Y - 0.3441 * (U - 128/255) - 0.7141 * (V - 128/255)
//   B = Y + 1.772  * (U - 128/255)
// ──────────────────────────────────────────────────────────────────────────────

// Desktop GLSL 1.20
static const char* kVertSrc_120 =
	"#version 120\n"
	"attribute vec2 aPos;\n"
	"attribute vec2 aTex;\n"
	"varying   vec2 vTex;\n"
	"void main() {\n"
	"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"    vTex = aTex;\n"
	"}\n";

static const char* kFragSrc_120 =
	"#version 120\n"
	"varying vec2 vTex;\n"
	"uniform sampler2D texY;\n"
	"uniform sampler2D texU;\n"
	"uniform sampler2D texV;\n"
	"void main() {\n"
	"    float y = texture2D(texY, vTex).r;\n"
	"    float u = texture2D(texU, vTex).r - 0.5;\n"
	"    float v = texture2D(texV, vTex).r - 0.5;\n"
	"    gl_FragColor = vec4(\n"
	"        y + 1.402   * v,\n"
	"        y - 0.34414 * u - 0.71414 * v,\n"
	"        y + 1.772   * u,\n"
	"        1.0);\n"
	"}\n";

// GLES 1.00 (softpipe / embedded)
static const char* kVertSrc_ES100 =
	"#version 100\n"
	"precision mediump float;\n"
	"attribute vec2 aPos;\n"
	"attribute vec2 aTex;\n"
	"varying   vec2 vTex;\n"
	"void main() {\n"
	"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"    vTex = aTex;\n"
	"}\n";

static const char* kFragSrc_ES100 =
	"#version 100\n"
	"precision mediump float;\n"
	"varying vec2 vTex;\n"
	"uniform sampler2D texY;\n"
	"uniform sampler2D texU;\n"
	"uniform sampler2D texV;\n"
	"void main() {\n"
	"    float y = texture2D(texY, vTex).r;\n"
	"    float u = texture2D(texU, vTex).r - 0.5;\n"
	"    float v = texture2D(texV, vTex).r - 0.5;\n"
	"    gl_FragColor = vec4(\n"
	"        y + 1.402   * v,\n"
	"        y - 0.34414 * u - 0.71414 * v,\n"
	"        y + 1.772   * u,\n"
	"        1.0);\n"
	"}\n";

/// Build the YUV→RGBA shader program and resolve locations.
/// Returns 0 on failure.
static GLuint create_yuv_program(bool use_es,
								 GLint& loc_aPos, GLint& loc_aTex,
								 GLint& loc_texY, GLint& loc_texU,
								 GLint& loc_texV) {
	const char* vsrc = use_es ? kVertSrc_ES100 : kVertSrc_120;
	const char* fsrc = use_es ? kFragSrc_ES100 : kFragSrc_120;

	DBGF("GL", "Creating YUV program (profile=%s)", use_es ? "ES100" : "120");

	GLuint vs = compile_shader(GL_VERTEX_SHADER, vsrc, "yuv_vert");
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc, "yuv_frag");
	if (vs == 0 || fs == 0) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512];
		GLsizei len = 0;
		glGetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
		std::cerr << "[OAVideoTexture][GL] program link failed: "
				  << std::string(log, (size_t)len) << std::endl;
		glDeleteProgram(prog);
		return 0;
	}

	loc_aPos = glGetAttribLocation(prog, "aPos");
	loc_aTex = glGetAttribLocation(prog, "aTex");
	loc_texY = glGetUniformLocation(prog, "texY");
	loc_texU = glGetUniformLocation(prog, "texU");
	loc_texV = glGetUniformLocation(prog, "texV");

	DBGF("GL", "YUV program=%u  aPos=%d aTex=%d  texY=%d texU=%d texV=%d",
		 prog, loc_aPos, loc_aTex, loc_texY, loc_texU, loc_texV);
	return prog;
}

// ──────────────────────────────────────────────────────────────────────────────
// GL resource init helpers (called lazily from populate)
// ──────────────────────────────────────────────────────────────────────────────

/// Ensure the output GL texture exists with correct filtering.
static void ensure_output_texture(OAVideoTexture* self) {
	if (self->gl_tex != 0) return;
	glGenTextures(1, &self->gl_tex);
	glBindTexture(GL_TEXTURE_2D, self->gl_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	DBGF("GL", "Created output texture id=%u", self->gl_tex);
}

/// Ensure all YUV→RGBA GL resources are initialized.
/// Returns false on shader compile/link failure.
static bool ensure_yuv_gl_resources(OAVideoTexture* self, bool use_es) {
	// Shader program
	if (self->program == 0) {
		self->program = create_yuv_program(
			use_es,
			self->loc_aPos, self->loc_aTex,
			self->loc_texY, self->loc_texU, self->loc_texV);
		if (self->program == 0) return false;
	}

	// Fullscreen quad VBO: 4 vertices × (pos.xy + uv.xy)
	if (self->vbo == 0) {
		static const GLfloat kQuad[] = {
			// pos       uv
			-1.f, -1.f,  0.f, 0.f,
			 1.f, -1.f,  1.f, 0.f,
			-1.f,  1.f,  0.f, 1.f,
			 1.f,  1.f,  1.f, 1.f,
		};
		glGenBuffers(1, &self->vbo);
		glBindBuffer(GL_ARRAY_BUFFER, self->vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
		DBGF("GL", "Created quad VBO id=%u  size=%zu bytes",
			 self->vbo, sizeof(kQuad));
	}

	// Plane textures
	if (self->y_tex == 0) {
		glGenTextures(1, &self->y_tex);
		DBGF("GL", "Created Y-plane texture id=%u", self->y_tex);
	}
	if (self->u_tex == 0) {
		glGenTextures(1, &self->u_tex);
		DBGF("GL", "Created U-plane texture id=%u", self->u_tex);
	}
	if (self->v_tex == 0) {
		glGenTextures(1, &self->v_tex);
		DBGF("GL", "Created V-plane texture id=%u", self->v_tex);
	}

	// FBO for rendering into output texture
	if (self->fbo == 0) {
		glGenFramebuffers(1, &self->fbo);
		DBGF("GL", "Created FBO id=%u", self->fbo);
	}

	self->gl_resources_inited = true;
	return true;
}

/// Upload a single-channel plane to a GL texture as RGBA.
/// The data is expanded R→RGBA on the CPU so the GPU stores 4 bytes/pixel.
/// This avoids a panfrost driver bug where tiled→linear conversion of
/// 1-bpp textures overflows the BO for certain dimensions (e.g. 800×480 R8).
/// Uses glTexSubImage2D to reuse existing storage when dimensions match,
/// avoiding the orphan+realloc cycle that exhausts the Mali CMA allocator.
static void upload_plane_texture(GLuint tex_id, int w, int h,
								 const guint8* data,
								 const char* label,
								 int& cached_w, int& cached_h) {
	// Expand single-channel → RGBA (value goes into R; A=0xFF).
	// Uses a persistent buffer to avoid per-frame heap churn.
	static std::vector<guint8> rgba_buf;
	const size_t pixel_count = (size_t)w * h;
	const size_t rgba_size   = pixel_count * 4;
	if (rgba_buf.size() < rgba_size)
		rgba_buf.resize(rgba_size);

	uint32_t* dst32 = reinterpret_cast<uint32_t*>(rgba_buf.data());
	for (size_t i = 0; i < pixel_count; ++i)
		dst32[i] = (uint32_t)data[i] | 0xFF000000u;

	glBindTexture(GL_TEXTURE_2D, tex_id);

	if (cached_w != w || cached_h != h) {
		// First upload or dimension change — allocate new storage
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, rgba_buf.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		cached_w = w;
		cached_h = h;
		DBGF("GL", "Allocated+uploaded %s plane: tex=%u  %dx%d  %zu bytes (RGBA)",
			 label, tex_id, w, h, rgba_size);
	} else {
		// Same dimensions — reuse existing backing store
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
						GL_RGBA, GL_UNSIGNED_BYTE, rgba_buf.data());
		DBGF("GL", "Sub-uploaded %s plane: tex=%u  %dx%d  %zu bytes (RGBA)",
			 label, tex_id, w, h, rgba_size);
	}

	if (log_gl_errors(label)) {
		std::cerr << "[OAVideoTexture][GL] error uploading " << label << std::endl;
	}
}

// ──────────────────────────────────────────────────────────────────────────────
// populate() — called by Flutter when it needs the texture content
// ──────────────────────────────────────────────────────────────────────────────

static gboolean oa_video_texture_populate(FlTextureGL* texture,
							uint32_t* target,
							uint32_t* name,
							uint32_t* width,
							uint32_t* height,
							GError** error) {
	OAVideoTexture* self = (OAVideoTexture*)texture;
	static std::atomic<int> frame_counter{0};
	static bool logged_fallback = false;

	// ── 1. Ensure output texture ──
	ensure_output_texture(self);
	glBindTexture(GL_TEXTURE_2D, self->gl_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	// ── 2. Snapshot state under mutex ──
	std::unique_lock<std::mutex> lk(self->mutex);

	const int cur_w = self->width  ? *self->width  : 0;
	const int cur_h = self->height ? *self->height : 0;

	const gsize rgba_needed = (gsize)cur_w * cur_h * 4;
	const bool have_rgba = (self->pixels && cur_w > 0 && cur_h > 0
							&& self->pixels->len >= rgba_needed);

	const gsize y_size  = (gsize)cur_w * cur_h;
	const gsize uv_w    = (cur_w + 1) / 2;
	const gsize uv_h    = (cur_h + 1) / 2;
	const gsize uv_size = uv_w * uv_h;
	const gsize yuv_needed = y_size + uv_size * 2;
	const bool have_yuv = (self->has_yuv && self->yuv && cur_w > 0 && cur_h > 0);

	// Query GL profile once per populate
	bool is_es = false;
	float glsl_version = 0.0f;
	query_glsl_profile(is_es, glsl_version);
	const bool use_es_shaders = is_es;

	// ── Fallback: red 1×1 pixel when no data available ──
	auto emit_fallback = [&]() {
		static const unsigned char red[] = {0xFF, 0x00, 0x00, 0xFF};
		glBindTexture(GL_TEXTURE_2D, self->gl_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, red);
		*width = 1;
		*height = 1;
		*target = GL_TEXTURE_2D;
		*name = self->gl_tex;
		if (!logged_fallback) {
			std::cout << "[OAVideoTexture] Using 1x1 fallback (no frame data)"
					  << std::endl;
			logged_fallback = true;
		}
	};

	// ── 3. YUV path ──
	if (have_yuv) {
		// Validate buffer bounds before touching data
		if (self->yuv->len < yuv_needed) {
			DBGF("POPULATE", "!!! YUV buffer too small: have=%u need=%zu  (overflow would occur)",
				 self->yuv->len, (size_t)yuv_needed);
			buf_track_read(self->dbg_yuv, self->yuv, 0, yuv_needed, "populate/yuv_bounds_check");
			lk.unlock();
			emit_fallback();
			return TRUE;
		}

		// Debug: track the read of all three planes
		buf_track_read(self->dbg_yuv, self->yuv, 0, y_size, "populate/Y_plane");
		buf_track_read(self->dbg_yuv, self->yuv, y_size, uv_size, "populate/U_plane");
		buf_track_read(self->dbg_yuv, self->yuv, y_size + uv_size, uv_size, "populate/V_plane");

		// Init GL resources (shader, VBO, plane textures, FBO)
		if (!ensure_yuv_gl_resources(self, use_es_shaders)) {
			DBGF("POPULATE", "Failed to init YUV GL resources — using fallback");
			lk.unlock();
			emit_fallback();
			return TRUE;
		}

		// Copy YUV data to a local buffer so we can release the mutex
		// before issuing GL calls (the driver's internal memcpy must not
		// race with set_yuv420p_frame reallocating self->yuv->data).
		std::vector<guint8> yuv_copy(self->yuv->data,
									 self->yuv->data + yuv_needed);
		const guint8* y_ptr = yuv_copy.data();
		const guint8* u_ptr = yuv_copy.data() + y_size;
		const guint8* v_ptr = yuv_copy.data() + y_size + uv_size;

		// Release mutex — GL operations below use only local data + GL handles
		lk.unlock();

		// Allocate or reuse RGBA output storage
		glBindTexture(GL_TEXTURE_2D, self->gl_tex);
		if (self->rgba_tex_w != cur_w || self->rgba_tex_h != cur_h) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cur_w, cur_h, 0,
						 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			self->rgba_tex_w = cur_w;
			self->rgba_tex_h = cur_h;
			DBGF("POPULATE", "RGBA output tex=%u allocated %dx%d (%zu bytes)",
				 self->gl_tex, cur_w, cur_h, (size_t)rgba_needed);
		}

		// Upload Y, U, V planes (reuses backing store when dimensions match)
		upload_plane_texture(self->y_tex, cur_w, cur_h, y_ptr,
							 "Y", self->y_tex_w, self->y_tex_h);
		upload_plane_texture(self->u_tex, (int)uv_w, (int)uv_h, u_ptr,
							 "U", self->u_tex_w, self->u_tex_h);
		upload_plane_texture(self->v_tex, (int)uv_w, (int)uv_h, v_ptr,
							 "V", self->v_tex_w, self->v_tex_h);

		// ── Draw fullscreen quad: YUV→RGBA ──
		glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							   GL_TEXTURE_2D, self->gl_tex, 0);

		GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
			DBGF("POPULATE", "!!! FBO incomplete (status=0x%x) — stale GL context? "
				 "Resetting GL resources.", fbo_status);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			// Invalidate all GL handles so they get recreated next frame
			self->fbo = 0; self->program = 0; self->vbo = 0;
			self->y_tex = 0; self->u_tex = 0; self->v_tex = 0;
			self->gl_tex = 0; self->gl_resources_inited = false;
			self->rgba_tex_w = 0; self->rgba_tex_h = 0;
			self->y_tex_w = 0; self->y_tex_h = 0;
			self->u_tex_w = 0; self->u_tex_h = 0;
			self->v_tex_w = 0; self->v_tex_h = 0;
			emit_fallback();
			return TRUE;
		}

		glViewport(0, 0, cur_w, cur_h);
		glUseProgram(self->program);

		// Bind plane textures to sampler units
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, self->y_tex);
		glUniform1i(self->loc_texY, 0);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, self->u_tex);
		glUniform1i(self->loc_texU, 1);

		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, self->v_tex);
		glUniform1i(self->loc_texV, 2);

		// Bind quad VBO and set vertex layout
		glBindBuffer(GL_ARRAY_BUFFER, self->vbo);
		glEnableVertexAttribArray(self->loc_aPos);
		glVertexAttribPointer(self->loc_aPos, 2, GL_FLOAT, GL_FALSE,
							  4 * sizeof(GLfloat), (const void*)0);
		glEnableVertexAttribArray(self->loc_aTex);
		glVertexAttribPointer(self->loc_aTex, 2, GL_FLOAT, GL_FALSE,
							  4 * sizeof(GLfloat),
							  (const void*)(2 * sizeof(GLfloat)));

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		// Ensure GPU finishes the YUV→RGBA conversion before we hand
		// the texture back to Flutter's compositor. Without this, Mali's
		// tile-based renderer queues unbounded work and the driver can
		// fault when recycling texture backing stores.
		glFinish();

		bool draw_ok = !log_gl_errors("yuv_draw");

		// Restore GL state
		glDisableVertexAttribArray(self->loc_aPos);
		glDisableVertexAttribArray(self->loc_aTex);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glUseProgram(0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		if (!draw_ok) {
			DBGF("POPULATE", "GL draw failed — using fallback");
			emit_fallback();
			return TRUE;
		}

		*width  = (uint32_t)cur_w;
		*height = (uint32_t)cur_h;
		int count = ++frame_counter;
		if (count <= 5 || count % 120 == 0) {
			std::cout << "[OAVideoTexture] YUV→GL " << cur_w << "x" << cur_h
					  << " (frame " << count << ")" << std::endl;
		}
		logged_fallback = false;

	// ── 4. RGBA path ──
	} else if (have_rgba) {
		// Validate buffer
		if (self->pixels->len < rgba_needed) {
			DBGF("POPULATE", "!!! RGBA buffer too small: have=%u need=%zu",
				 self->pixels->len, (size_t)rgba_needed);
			buf_track_read(self->dbg_pixels, self->pixels, 0, rgba_needed,
						   "populate/rgba_bounds_check");
			lk.unlock();
			emit_fallback();
			return TRUE;
		}

		buf_track_read(self->dbg_pixels, self->pixels, 0, rgba_needed, "populate/rgba");

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cur_w, cur_h, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, self->pixels->data);

		DBGF("POPULATE", "RGBA upload tex=%u  %dx%d  %zu bytes",
			 self->gl_tex, cur_w, cur_h, (size_t)rgba_needed);

		*width  = (uint32_t)cur_w;
		*height = (uint32_t)cur_h;
		int count = ++frame_counter;
		if (count <= 5 || count % 120 == 0) {
			std::cout << "[OAVideoTexture] RGBA→GL " << cur_w << "x" << cur_h
					  << " (frame " << count << ")" << std::endl;
		}
		logged_fallback = false;
		lk.unlock();

	// ── 5. No data — fallback ──
	} else {
		DBGF("POPULATE", "No frame data: w=%d h=%d  has_yuv=%d  yuv=%p(len=%u)  pixels=%p(len=%u)",
			 cur_w, cur_h, (int)self->has_yuv,
			 (void*)self->yuv, self->yuv ? self->yuv->len : 0,
			 (void*)self->pixels, self->pixels ? self->pixels->len : 0);
		lk.unlock();
		emit_fallback();
	}

	*target = GL_TEXTURE_2D;
	*name   = self->gl_tex;
	return TRUE;
}

// ──────────────────────────────────────────────────────────────────────────────
// dispose — release CPU-side resources (GL objects freed by context teardown)
// ──────────────────────────────────────────────────────────────────────────────

static void oa_video_texture_dispose(GObject* obj) {
	OAVideoTexture* self = OA_VIDEO_TEXTURE(obj);
	DBGF("LIFECYCLE", "dispose() called  gl_tex=%u  pixels=%p  yuv=%p  w=%p h=%p",
		 self->gl_tex,
		 (void*)self->pixels, (void*)self->yuv,
		 (void*)self->width, (void*)self->height);

	// GL objects are NOT deleted here — no GL context may be current.
	// They are cleaned up when the GL context is torn down.
	if (self->gl_tex != 0) {
		DBGF("LIFECYCLE", "Abandoning GL resources: tex=%u y=%u u=%u v=%u fbo=%u program=%u vbo=%u",
			 self->gl_tex, self->y_tex, self->u_tex, self->v_tex,
			 self->fbo, self->program, self->vbo);
	}
	self->gl_tex = 0;
	self->y_tex = 0;
	self->u_tex = 0;
	self->v_tex = 0;
	self->fbo = 0;
	self->program = 0;
	self->vbo = 0;
	self->gl_resources_inited = false;
	self->rgba_tex_w = 0; self->rgba_tex_h = 0;
	self->y_tex_w = 0; self->y_tex_h = 0;
	self->u_tex_w = 0; self->u_tex_h = 0;
	self->v_tex_w = 0; self->v_tex_h = 0;

	if (self->pixels) {
		buf_track_free(self->dbg_pixels, "dispose/pixels");
		g_byte_array_unref(self->pixels);
		self->pixels = nullptr;
	}
	if (self->yuv) {
		buf_track_free(self->dbg_yuv, "dispose/yuv");
		g_byte_array_unref(self->yuv);
		self->yuv = nullptr;
	}
	if (self->width) {
		DBGF("LIFECYCLE", "Freeing width ptr=%p  val=%d", (void*)self->width, *self->width);
		g_free(self->width);
		self->width = nullptr;
	}
	if (self->height) {
		DBGF("LIFECYCLE", "Freeing height ptr=%p  val=%d", (void*)self->height, *self->height);
		g_free(self->height);
		self->height = nullptr;
	}

	DBGF("LIFECYCLE", "dispose() done — calling parent dispose");
	G_OBJECT_CLASS(oa_video_texture_parent_class)->dispose(obj);
}

// ──────────────────────────────────────────────────────────────────────────────
// GObject class/instance init
// ──────────────────────────────────────────────────────────────────────────────

static void oa_video_texture_class_init(OAVideoTextureClass* klass) {
	FL_TEXTURE_GL_CLASS(klass)->populate = oa_video_texture_populate;
	G_OBJECT_CLASS(klass)->dispose = oa_video_texture_dispose;
}

static void oa_video_texture_init(OAVideoTexture* self) {
	self->pixels = g_byte_array_new();
	self->yuv    = g_byte_array_new();

	buf_track_init(self->dbg_pixels, "pixels", self->pixels);
	buf_track_init(self->dbg_yuv, "yuv", self->yuv);

	DBGF("LIFECYCLE", "init()  pixels=%p(len=%u)  yuv=%p(len=%u)",
		 (void*)self->pixels, self->pixels->len,
		 (void*)self->yuv, self->yuv->len);
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────

OAVideoTexture* oa_video_texture_new(int width, int height) {
	OAVideoTexture* self = OA_VIDEO_TEXTURE(
		g_object_new(OA_VIDEO_TEXTURE_TYPE, nullptr));

	self->width  = g_new(int, 1);
	self->height = g_new(int, 1);
	*self->width  = width;
	*self->height = height;

	const gsize cap = (width > 0 && height > 0) ? (gsize)width * height * 4 : 0;
	if (cap > 0) {
		g_byte_array_set_size(self->pixels, cap);
		buf_track_resize(self->dbg_pixels, self->pixels, cap, "oa_video_texture_new");
	}

	DBGF("LIFECYCLE", "new(%d, %d)  initial_rgba_cap=%zu  pixels=%p  yuv=%p",
		 width, height, (size_t)cap,
		 (void*)self->pixels, (void*)self->yuv);
	return self;
}

int64_t oa_video_texture_register(OAVideoTexture* self,
								  FlTextureRegistrar* registrar) {
	if (self->registered_id != 0) {
		DBGF("LIFECYCLE", "register() — already registered id=%" PRId64,
			 self->registered_id);
		return self->registered_id;
	}

	FlTexture* base = FL_TEXTURE(self);
	gboolean ok = fl_texture_registrar_register_texture(registrar, base);
	if (!ok) {
		DBGF("LIFECYCLE", "register() — fl_texture_registrar_register_texture FAILED");
		self->registered_id = 0;
		return 0;
	}
	self->registered_id = fl_texture_get_id(base);
	DBGF("LIFECYCLE", "register() — success  texture_id=%" PRId64, self->registered_id);
	return self->registered_id;
}

void oa_video_texture_set_frame(OAVideoTexture* self,
								const guint8* rgba_bytes,
								gsize length,
								int width,
								int height) {
	std::lock_guard<std::mutex> lk(self->mutex);

	// Ensure dimension storage
	if (!self->width)  self->width  = g_new(int, 1);
	if (!self->height) self->height = g_new(int, 1);
	*self->width  = width;
	*self->height = height;

	const gsize needed = (gsize)width * height * 4u;

	DBGF("SET_FRAME", "RGBA set_frame(%dx%d)  needed=%zu  input_len=%zu  buf_alloc=%u",
		 width, height, (size_t)needed, (size_t)length,
		 self->pixels ? self->pixels->len : 0);

	// Check for use-after-free
	if (!self->pixels) {
		DBGF("SET_FRAME", "!!! pixels buffer is NULL (use-after-free?)");
		return;
	}

	// Resize if needed
	if (self->pixels->len != needed) {
		buf_track_resize(self->dbg_pixels, self->pixels, needed, "set_frame");
		g_byte_array_set_size(self->pixels, needed);
	}

	// Validate input and copy
	if (rgba_bytes && length >= needed) {
		buf_track_write(self->dbg_pixels, self->pixels, 0, needed, "set_frame/memcpy");
		memcpy(self->pixels->data, rgba_bytes, needed);
	} else {
		DBGF("SET_FRAME", "!!! input too small or null: rgba_bytes=%p length=%zu needed=%zu",
			 (const void*)rgba_bytes, (size_t)length, (size_t)needed);
	}

	self->has_yuv = FALSE;
}

void oa_video_texture_set_yuv420p_frame(OAVideoTexture* self,
										const guint8* yuv_bytes,
										gsize length,
										int width,
										int height) {
	std::lock_guard<std::mutex> lk(self->mutex);

	// Ensure dimension storage
	if (!self->width)  self->width  = g_new(int, 1);
	if (!self->height) self->height = g_new(int, 1);
	*self->width  = width;
	*self->height = height;

	const gsize y_size  = (gsize)width * height;
	const gsize uv_w    = (width + 1) / 2;
	const gsize uv_h    = (height + 1) / 2;
	const gsize uv_size = uv_w * uv_h;
	const gsize needed  = y_size + uv_size * 2;

	DBGF("SET_FRAME", "YUV set_yuv420p_frame(%dx%d)  Y=%zu U=%zu V=%zu  total_needed=%zu  input_len=%zu  buf_alloc=%u",
		 width, height,
		 (size_t)y_size, (size_t)uv_size, (size_t)uv_size,
		 (size_t)needed, (size_t)length,
		 self->yuv ? self->yuv->len : 0);

	// Check for use-after-free
	if (!self->yuv) {
		DBGF("SET_FRAME", "!!! yuv buffer is NULL (use-after-free?)");
		self->has_yuv = FALSE;
		return;
	}

	// Resize if needed
	if (self->yuv->len != needed) {
		buf_track_resize(self->dbg_yuv, self->yuv, needed, "set_yuv420p_frame");
		g_byte_array_set_size(self->yuv, needed);
	}

	// Validate input and copy
	if (yuv_bytes && length >= needed) {
		buf_track_write(self->dbg_yuv, self->yuv, 0, needed, "set_yuv420p_frame/memcpy");
		memcpy(self->yuv->data, yuv_bytes, needed);
		self->has_yuv = TRUE;
	} else {
		DBGF("SET_FRAME", "!!! input too small or null: yuv_bytes=%p length=%zu needed=%zu — marking has_yuv=FALSE",
			 (const void*)yuv_bytes, (size_t)length, (size_t)needed);
		self->has_yuv = FALSE;
	}
}

void oa_video_texture_mark_frame_available(OAVideoTexture* self,
										   FlTextureRegistrar* registrar) {
	DBGF("FRAME", "mark_frame_available() id=%" PRId64, self->registered_id);
	fl_texture_registrar_mark_texture_frame_available(registrar, FL_TEXTURE(self));
}

