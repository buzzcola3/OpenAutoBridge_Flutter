#include "oa_video_texture.h"

#include <epoxy/gl.h>
#include <flutter_linux/flutter_linux.h>
#include <cstring>
#include <mutex>
#include <vector>

// ──────────────────────────────────────────────────────────────────────────────
// Instance struct
// ──────────────────────────────────────────────────────────────────────────────
struct _OAVideoTexture {
	FlTextureGL parent_instance;

	GLuint gl_tex  = 0;          // output RGBA texture
	int    cur_w   = 0;          // current frame width
	int    cur_h   = 0;          // current frame height

	// YUV420P packed buffer: [Y plane][U plane][V plane]
	std::vector<guint8> yuv;
	bool has_yuv = false;

	// GL resources for YUV→RGBA conversion
	GLuint y_tex   = 0;
	GLuint u_tex   = 0;
	GLuint v_tex   = 0;
	GLuint fbo     = 0;
	GLuint program = 0;
	GLuint vbo     = 0;

	// Shader locations
	GLint loc_aPos = -1;
	GLint loc_aTex = -1;
	GLint loc_texY = -1;
	GLint loc_texU = -1;
	GLint loc_texV = -1;

	int64_t registered_id = 0;

	// Cached texture dimensions (avoid glTexImage2D realloc when unchanged)
	int rgba_tex_w = 0, rgba_tex_h = 0;
	int y_tex_w = 0, y_tex_h = 0;
	int u_tex_w = 0, u_tex_h = 0;
	int v_tex_w = 0, v_tex_h = 0;

	// RGBA staging buffer for R→RGBA plane expansion (reused across frames)
	std::vector<uint32_t> plane_staging;

	// Cached GLSL profile
	bool profile_queried = false;
	bool use_es          = false;

	std::mutex mutex;  // protects yuv, cur_w, cur_h, has_yuv
};

struct _OAVideoTextureClass {
	FlTextureGLClass parent_class;
};

G_DEFINE_TYPE(OAVideoTexture, oa_video_texture, fl_texture_gl_get_type())

// ──────────────────────────────────────────────────────────────────────────────
// GL helpers
// ──────────────────────────────────────────────────────────────────────────────

static bool check_gl_errors(const char* stage) {
	bool had = false;
	for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) {
		had = true;
		fprintf(stderr, "[OAVideoTexture] GL error 0x%x at %s\n", e, stage);
	}
	return had;
}

static GLuint compile_shader(GLenum type, const char* src, const char* label) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		fprintf(stderr, "[OAVideoTexture] %s compile failed: %s\n", label, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static void query_gl_profile(OAVideoTexture* self) {
	if (self->profile_queried) return;
	self->profile_queried = true;
	const char* sl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	self->use_es = sl && strstr(sl, "ES");
}

// ──────────────────────────────────────────────────────────────────────────────
// YUV→RGBA shaders (BT.601 limited-range)
// ──────────────────────────────────────────────────────────────────────────────

static const char* kVert_120 =
	"#version 120\n"
	"attribute vec2 aPos;\n"
	"attribute vec2 aTex;\n"
	"varying   vec2 vTex;\n"
	"void main() {\n"
	"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"    vTex = aTex;\n"
	"}\n";

static const char* kFrag_120 =
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

static const char* kVert_ES100 =
	"#version 100\n"
	"precision mediump float;\n"
	"attribute vec2 aPos;\n"
	"attribute vec2 aTex;\n"
	"varying   vec2 vTex;\n"
	"void main() {\n"
	"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"    vTex = aTex;\n"
	"}\n";

static const char* kFrag_ES100 =
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

// ──────────────────────────────────────────────────────────────────────────────
// GL resource setup (lazy, on first populate)
// ──────────────────────────────────────────────────────────────────────────────

static bool ensure_program(OAVideoTexture* self) {
	if (self->program) return true;

	const char* vs = self->use_es ? kVert_ES100 : kVert_120;
	const char* fs = self->use_es ? kFrag_ES100 : kFrag_120;

	GLuint v = compile_shader(GL_VERTEX_SHADER, vs, "yuv_vert");
	GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs, "yuv_frag");
	if (!v || !f) {
		if (v) glDeleteShader(v);
		if (f) glDeleteShader(f);
		return false;
	}

	GLuint p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glLinkProgram(p);
	glDeleteShader(v);
	glDeleteShader(f);

	GLint linked = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512];
		glGetProgramInfoLog(p, sizeof(log), nullptr, log);
		fprintf(stderr, "[OAVideoTexture] link failed: %s\n", log);
		glDeleteProgram(p);
		return false;
	}

	self->program  = p;
	self->loc_aPos = glGetAttribLocation(p, "aPos");
	self->loc_aTex = glGetAttribLocation(p, "aTex");
	self->loc_texY = glGetUniformLocation(p, "texY");
	self->loc_texU = glGetUniformLocation(p, "texU");
	self->loc_texV = glGetUniformLocation(p, "texV");
	return true;
}

static void ensure_vbo(OAVideoTexture* self) {
	if (self->vbo) return;
	static const GLfloat quad[] = {
		-1.f, -1.f,  0.f, 0.f,
		 1.f, -1.f,  1.f, 0.f,
		-1.f,  1.f,  0.f, 1.f,
		 1.f,  1.f,  1.f, 1.f,
	};
	glGenBuffers(1, &self->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, self->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
}

static void ensure_tex(GLuint& id) {
	if (id) return;
	glGenTextures(1, &id);
}

static void ensure_fbo(GLuint& id) {
	if (id) return;
	glGenFramebuffers(1, &id);
}

// ──────────────────────────────────────────────────────────────────────────────
// R→RGBA expansion + upload
//
// Each single-channel byte is packed as 0xFFxxxxRR (little-endian RGBA with
// the value in R, G=B=0, A=0xFF).  The 4-bpp RGBA internal format sidesteps
// a Mesa panfrost bug where tiled→linear conversion of 1-bpp textures can
// overflow the backing BO.
// ──────────────────────────────────────────────────────────────────────────────

static void upload_plane(OAVideoTexture* self, GLuint tex,
						 int w, int h, const guint8* data,
						 int& cw, int& ch) {
	const size_t n = (size_t)w * h;
	if (self->plane_staging.size() < n)
		self->plane_staging.resize(n);

	uint32_t* dst = self->plane_staging.data();
	size_t i = 0;
	const size_t n4 = n & ~(size_t)3;
	for (; i < n4; i += 4) {
		dst[i + 0] = (uint32_t)data[i + 0] | 0xFF000000u;
		dst[i + 1] = (uint32_t)data[i + 1] | 0xFF000000u;
		dst[i + 2] = (uint32_t)data[i + 2] | 0xFF000000u;
		dst[i + 3] = (uint32_t)data[i + 3] | 0xFF000000u;
	}
	for (; i < n; ++i)
		dst[i] = (uint32_t)data[i] | 0xFF000000u;

	glBindTexture(GL_TEXTURE_2D, tex);
	if (cw != w || ch != h) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, dst);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		cw = w; ch = h;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
						GL_RGBA, GL_UNSIGNED_BYTE, dst);
	}
}

// ──────────────────────────────────────────────────────────────────────────────
// Reset all cached GL handles (after context loss / FBO error)
// ──────────────────────────────────────────────────────────────────────────────

static void invalidate_gl(OAVideoTexture* self) {
	self->gl_tex = self->fbo = self->program = self->vbo = 0;
	self->y_tex = self->u_tex = self->v_tex = 0;
	self->rgba_tex_w = self->rgba_tex_h = 0;
	self->y_tex_w = self->y_tex_h = 0;
	self->u_tex_w = self->u_tex_h = 0;
	self->v_tex_w = self->v_tex_h = 0;
	self->profile_queried = false;
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

	auto emit_fallback = [&]() {
		ensure_tex(self->gl_tex);
		static const guint8 red[] = {0xFF, 0x00, 0x00, 0xFF};
		glBindTexture(GL_TEXTURE_2D, self->gl_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, red);
		*width = 1; *height = 1;
		*target = GL_TEXTURE_2D; *name = self->gl_tex;
	};

	// ── Snapshot and validate under mutex ──
	std::unique_lock<std::mutex> lk(self->mutex);
	const int w = self->cur_w;
	const int h = self->cur_h;
	if (!self->has_yuv || w <= 0 || h <= 0) {
		lk.unlock();
		emit_fallback();
		return TRUE;
	}

	const size_t y_size  = (size_t)w * h;
	const int    uv_w    = (w + 1) / 2;
	const int    uv_h    = (h + 1) / 2;
	const size_t uv_size = (size_t)uv_w * uv_h;
	const size_t needed  = y_size + uv_size * 2;
	if (self->yuv.size() < needed) {
		lk.unlock();
		emit_fallback();
		return TRUE;
	}

	const guint8* y_ptr = self->yuv.data();
	const guint8* u_ptr = self->yuv.data() + y_size;
	const guint8* v_ptr = self->yuv.data() + y_size + uv_size;

	// ── Ensure GL resources ──
	query_gl_profile(self);
	ensure_tex(self->gl_tex);
	if (!ensure_program(self)) {
		lk.unlock();
		emit_fallback();
		return TRUE;
	}
	ensure_vbo(self);
	ensure_tex(self->y_tex);
	ensure_tex(self->u_tex);
	ensure_tex(self->v_tex);
	ensure_fbo(self->fbo);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

	// ── Allocate/reuse RGBA output texture ──
	glBindTexture(GL_TEXTURE_2D, self->gl_tex);
	if (self->rgba_tex_w != w || self->rgba_tex_h != h) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		self->rgba_tex_w = w;
		self->rgba_tex_h = h;
	}

	// ── Upload Y, U, V planes (expand R→RGBA, reads yuv under mutex) ──
	upload_plane(self, self->y_tex, w, h, y_ptr,
				 self->y_tex_w, self->y_tex_h);
	upload_plane(self, self->u_tex, uv_w, uv_h, u_ptr,
				 self->u_tex_w, self->u_tex_h);
	upload_plane(self, self->v_tex, uv_w, uv_h, v_ptr,
				 self->v_tex_w, self->v_tex_h);

	// Done reading YUV data
	lk.unlock();

	// ── FBO draw: YUV→RGBA ──
	glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
						   GL_TEXTURE_2D, self->gl_tex, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "[OAVideoTexture] FBO incomplete — resetting GL state\n");
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		invalidate_gl(self);
		emit_fallback();
		return TRUE;
	}

	glViewport(0, 0, w, h);
	glUseProgram(self->program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, self->y_tex);
	glUniform1i(self->loc_texY, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, self->u_tex);
	glUniform1i(self->loc_texU, 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, self->v_tex);
	glUniform1i(self->loc_texV, 2);

	glBindBuffer(GL_ARRAY_BUFFER, self->vbo);
	glEnableVertexAttribArray(self->loc_aPos);
	glVertexAttribPointer(self->loc_aPos, 2, GL_FLOAT, GL_FALSE,
						  4 * sizeof(GLfloat), (const void*)0);
	glEnableVertexAttribArray(self->loc_aTex);
	glVertexAttribPointer(self->loc_aTex, 2, GL_FLOAT, GL_FALSE,
						  4 * sizeof(GLfloat), (const void*)(2 * sizeof(GLfloat)));

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(self->loc_aPos);
	glDisableVertexAttribArray(self->loc_aTex);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (check_gl_errors("yuv_draw")) {
		emit_fallback();
		return TRUE;
	}

	*target = GL_TEXTURE_2D;
	*name   = self->gl_tex;
	*width  = (uint32_t)w;
	*height = (uint32_t)h;
	return TRUE;
}

// ──────────────────────────────────────────────────────────────────────────────
// dispose
// ──────────────────────────────────────────────────────────────────────────────

static void oa_video_texture_dispose(GObject* obj) {
	OAVideoTexture* self = OA_VIDEO_TEXTURE(obj);
	invalidate_gl(self);
	self->yuv.clear();
	self->yuv.shrink_to_fit();
	self->plane_staging.clear();
	self->plane_staging.shrink_to_fit();
	G_OBJECT_CLASS(oa_video_texture_parent_class)->dispose(obj);
}

// ──────────────────────────────────────────────────────────────────────────────
// GObject init
// ──────────────────────────────────────────────────────────────────────────────

static void oa_video_texture_class_init(OAVideoTextureClass* klass) {
	FL_TEXTURE_GL_CLASS(klass)->populate = oa_video_texture_populate;
	G_OBJECT_CLASS(klass)->dispose = oa_video_texture_dispose;
}

static void oa_video_texture_init(OAVideoTexture*) {}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────

OAVideoTexture* oa_video_texture_new(int width, int height) {
	OAVideoTexture* self = OA_VIDEO_TEXTURE(
		g_object_new(OA_VIDEO_TEXTURE_TYPE, nullptr));
	self->cur_w = width;
	self->cur_h = height;
	return self;
}

int64_t oa_video_texture_register(OAVideoTexture* self,
								  FlTextureRegistrar* registrar) {
	if (self->registered_id != 0)
		return self->registered_id;
	if (!fl_texture_registrar_register_texture(registrar, FL_TEXTURE(self)))
		return 0;
	self->registered_id = fl_texture_get_id(FL_TEXTURE(self));
	return self->registered_id;
}

void oa_video_texture_set_yuv420p_frame(OAVideoTexture* self,
										const guint8* yuv_bytes,
										gsize length,
										int width,
										int height) {
	const size_t y_sz  = (size_t)width * height;
	const size_t uv_w  = (width + 1) / 2;
	const size_t uv_h  = (height + 1) / 2;
	const size_t uv_sz = uv_w * uv_h;
	const size_t needed = y_sz + uv_sz * 2;
	if (!yuv_bytes || length < needed)
		return;

	std::lock_guard<std::mutex> lk(self->mutex);
	self->cur_w = width;
	self->cur_h = height;
	self->yuv.resize(needed);
	memcpy(self->yuv.data(), yuv_bytes, needed);
	self->has_yuv = true;
}

void oa_video_texture_mark_frame_available(OAVideoTexture* self,
										   FlTextureRegistrar* registrar) {
	fl_texture_registrar_mark_texture_frame_available(registrar, FL_TEXTURE(self));
}
