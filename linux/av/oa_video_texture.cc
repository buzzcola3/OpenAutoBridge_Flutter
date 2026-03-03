// OAVideoTexture — Flutter GL texture fed from a DropBuffer.
//
// Handles three frame formats:
//   1. NV12 DMA-BUF  → EGL import per-plane (R8 + GR88) → NV12→RGB shader
//   2. NV12 CPU      → glTexImage2D Y (R8) + UV (RG8)   → NV12→RGB shader
//   3. RGBA CPU      → glTexImage2D / glTexSubImage2D
//
// All GL work happens on Flutter's raster thread (populate callback).

#include "oa_video_texture.h"

#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <flutter_linux/flutter_linux.h>
#include <cstring>
#include <iostream>
#include <unistd.h>

// GL error checking (matches cedrus gl_check_errors)
static const char* gl_error_str(GLenum e) {
    switch (e) {
        case GL_NO_ERROR:          return "GL_NO_ERROR";
        case GL_INVALID_ENUM:      return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:     return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY:     return "GL_OUT_OF_MEMORY";
        default:                   return "UNKNOWN";
    }
}

static void gl_check_errors(const char* where) {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {
        std::cerr << "[OAVideoTexture] GL ERROR at " << where
                  << ": " << gl_error_str(e)
                  << " (0x" << std::hex << e << std::dec << ")\n";
    }
}

// DRM fourcc values (may not be in headers on all systems)
#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8    0x20203852
#endif
#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88  0x38385247
#endif
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12  0x3231564E
#endif

// ──────────────────────────────────────────────────────────────────────────────
// NV12 → RGB shader sources
// ──────────────────────────────────────────────────────────────────────────────

static const char* kNV12Vert = R"glsl(
#version 100
attribute vec2 aPos;
attribute vec2 aTex;
varying   vec2 vTex;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); vTex = aTex; }
)glsl";

static const char* kNV12Frag = R"glsl(
#version 100
precision mediump float;
varying vec2 vTex;
uniform sampler2D texY;
uniform sampler2D texUV;
void main() {
    float y  = texture2D(texY,  vTex).r;
    vec2  uv = texture2D(texUV, vTex).rg;
    float u  = uv.x - 0.5;
    float v  = uv.y - 0.5;
    gl_FragColor = vec4(
        y + 1.402 * v,
        y - 0.34414 * u - 0.71414 * v,
        y + 1.772 * u,
        1.0);
}
)glsl";

// ──────────────────────────────────────────────────────────────────────────────
// Shader / FBO helpers
// ──────────────────────────────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src, const char* label) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "[OAVideoTexture] shader compile failed (" << label << "): " << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[OAVideoTexture] program link failed: " << log << "\n";
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ──────────────────────────────────────────────────────────────────────────────
// Instance struct
// ──────────────────────────────────────────────────────────────────────────────

struct _OAVideoTexture {
    FlTextureGL parent_instance;

    DropBuffer* buf;

    // Output RGBA texture (final result presented to Flutter)
    GLuint gl_tex   = 0;
    int    tex_w    = 0;
    int    tex_h    = 0;
    int64_t registered_id = 0;

    FrameSlot* prev_slot = nullptr;

    // NV12 → RGB conversion resources
    GLuint nv12_program = 0;
    GLuint nv12_fbo     = 0;     // off-screen FBO to render NV12→RGB into gl_tex
    GLuint nv12_vbo     = 0;
    GLuint y_tex        = 0;
    GLuint uv_tex       = 0;
    GLint  loc_aPos     = -1;
    GLint  loc_aTex     = -1;
    GLint  loc_texY     = -1;
    GLint  loc_texUV    = -1;
    int    y_tex_w      = 0;
    int    y_tex_h      = 0;
    int    uv_tex_w     = 0;
    int    uv_tex_h     = 0;

    // EGL DMA-BUF import function pointers (resolved once)
    bool   egl_probed        = false;
    bool   dmabuf_supported  = false;
    PFNEGLCREATEIMAGEKHRPROC       eglCreateImageKHR_fn   = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC      eglDestroyImageKHR_fn  = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_fn = nullptr;
    EGLImageKHR prev_y_img   = EGL_NO_IMAGE_KHR;
    EGLImageKHR prev_uv_img  = EGL_NO_IMAGE_KHR;

    bool nv12_inited = false;
    bool logged_path = false;
};

struct _OAVideoTextureClass {
    FlTextureGLClass parent_class;
};

G_DEFINE_TYPE(OAVideoTexture, oa_video_texture, fl_texture_gl_get_type())

// ──────────────────────────────────────────────────────────────────────────────
// One-time NV12 shader / FBO init
// ──────────────────────────────────────────────────────────────────────────────

static void ensure_nv12_resources(OAVideoTexture* self) {
    if (self->nv12_inited) return;

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kNV12Vert, "nv12-vert");
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kNV12Frag, "nv12-frag");
    if (!vs || !fs) return;

    self->nv12_program = link_program(vs, fs);
    if (!self->nv12_program) return;

    self->loc_aPos  = glGetAttribLocation(self->nv12_program,  "aPos");
    self->loc_aTex  = glGetAttribLocation(self->nv12_program,  "aTex");
    self->loc_texY  = glGetUniformLocation(self->nv12_program, "texY");
    self->loc_texUV = glGetUniformLocation(self->nv12_program, "texUV");

    // Full-screen quad VBO
    static const GLfloat quad[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    glGenBuffers(1, &self->nv12_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, self->nv12_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glGenTextures(1, &self->y_tex);
    glGenTextures(1, &self->uv_tex);
    glGenFramebuffers(1, &self->nv12_fbo);

    self->nv12_inited = true;
    gl_check_errors("ensure_nv12_resources");
}

// ──────────────────────────────────────────────────────────────────────────────
// One-time EGL DMA-BUF capability probe
// ──────────────────────────────────────────────────────────────────────────────

static void probe_egl_dmabuf(OAVideoTexture* self) {
    if (self->egl_probed) return;
    self->egl_probed = true;

    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy == EGL_NO_DISPLAY) {
        std::cout << "[OAVideoTexture] no EGL display — DMA-BUF import disabled\n";
        return;
    }

    const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
    auto has_ext = [&](const char* name) -> bool {
        if (!exts) return false;
        return strstr(exts, name) != nullptr;
    };

    if (!has_ext("EGL_EXT_image_dma_buf_import")) {
        std::cout << "[OAVideoTexture] EGL_EXT_image_dma_buf_import not available\n";
        return;
    }

    const char* gl_exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    auto has_gl_ext = [&](const char* name) -> bool {
        if (!gl_exts) return false;
        return strstr(gl_exts, name) != nullptr;
    };
    if (!has_gl_ext("GL_OES_EGL_image")) {
        std::cout << "[OAVideoTexture] GL_OES_EGL_image not available\n";
        return;
    }

    self->eglCreateImageKHR_fn = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    self->eglDestroyImageKHR_fn = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    self->glEGLImageTargetTexture2DOES_fn = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (self->eglCreateImageKHR_fn && self->eglDestroyImageKHR_fn &&
        self->glEGLImageTargetTexture2DOES_fn) {
        self->dmabuf_supported = true;
    }

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    std::cout << "[OAVideoTexture] GL: " << (renderer ? renderer : "unknown")
              << " | DMA-BUF: " << (self->dmabuf_supported ? "yes" : "no") << "\n";
    gl_check_errors("probe_egl_dmabuf");
}

// ──────────────────────────────────────────────────────────────────────────────
// EGL DMA-BUF import helper
// ──────────────────────────────────────────────────────────────────────────────

static EGLImageKHR import_dmabuf_plane(OAVideoTexture* self,
                                       int fd, uint32_t fourcc,
                                       uint32_t w, uint32_t h,
                                       uint32_t stride, uint32_t offset) {
    EGLDisplay dpy = eglGetCurrentDisplay();
    const EGLint attrs[] = {
        EGL_WIDTH,  static_cast<EGLint>(w),
        EGL_HEIGHT, static_cast<EGLint>(h),
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT,     fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(stride),
        EGL_NONE,
    };
    return self->eglCreateImageKHR_fn(dpy, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT,
                                      nullptr, attrs);
}

// ──────────────────────────────────────────────────────────────────────────────
// Upload Y/UV textures via CPU or DMA-BUF import
// ──────────────────────────────────────────────────────────────────────────────

static bool upload_nv12_dmabuf(OAVideoTexture* self, FrameSlot* slot) {
    probe_egl_dmabuf(self);
    if (!self->dmabuf_supported) return false;

    EGLDisplay dpy = eglGetCurrentDisplay();

    // Destroy previous frame's EGLImages
    if (self->prev_y_img != EGL_NO_IMAGE_KHR) {
        self->eglDestroyImageKHR_fn(dpy, self->prev_y_img);
        self->prev_y_img = EGL_NO_IMAGE_KHR;
    }
    if (self->prev_uv_img != EGL_NO_IMAGE_KHR) {
        self->eglDestroyImageKHR_fn(dpy, self->prev_uv_img);
        self->prev_uv_img = EGL_NO_IMAGE_KHR;
    }

    const uint32_t w = static_cast<uint32_t>(slot->width);
    const uint32_t h = static_cast<uint32_t>(slot->height);
    const uint32_t uv_w = (w + 1) / 2;
    const uint32_t uv_h = (h + 1) / 2;

    // Import Y plane as R8
    EGLImageKHR y_img = import_dmabuf_plane(self,
        slot->y_dma_fd, DRM_FORMAT_R8, w, h,
        slot->y_dma_stride, slot->y_dma_offset);
    if (y_img == EGL_NO_IMAGE_KHR) {
        std::cout << "[OAVideoTexture] Y plane EGL import FAILED: fd="
                  << slot->y_dma_fd << " " << w << "x" << h
                  << " stride=" << slot->y_dma_stride
                  << " offset=" << slot->y_dma_offset
                  << " eglError=0x" << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }

    // Import UV plane as GR88
    EGLImageKHR uv_img = import_dmabuf_plane(self,
        slot->uv_dma_fd, DRM_FORMAT_GR88, uv_w, uv_h,
        slot->uv_dma_stride, slot->uv_dma_offset);
    if (uv_img == EGL_NO_IMAGE_KHR) {
        std::cout << "[OAVideoTexture] UV plane EGL import FAILED: fd="
                  << slot->uv_dma_fd << " " << uv_w << "x" << uv_h
                  << " stride=" << slot->uv_dma_stride
                  << " offset=" << slot->uv_dma_offset
                  << " eglError=0x" << std::hex << eglGetError() << std::dec << "\n";
        self->eglDestroyImageKHR_fn(dpy, y_img);
        return false;
    }

    // Bind to Y texture
    glBindTexture(GL_TEXTURE_2D, self->y_tex);
    self->glEGLImageTargetTexture2DOES_fn(GL_TEXTURE_2D, y_img);
    gl_check_errors("dmabuf Y glEGLImageTargetTexture2DOES");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Bind to UV texture
    glBindTexture(GL_TEXTURE_2D, self->uv_tex);
    self->glEGLImageTargetTexture2DOES_fn(GL_TEXTURE_2D, uv_img);
    gl_check_errors("dmabuf UV glEGLImageTargetTexture2DOES");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    self->prev_y_img  = y_img;
    self->prev_uv_img = uv_img;

    self->y_tex_w  = static_cast<int>(w);
    self->y_tex_h  = static_cast<int>(h);
    self->uv_tex_w = static_cast<int>(uv_w);
    self->uv_tex_h = static_cast<int>(uv_h);

    return true;
}

static void upload_nv12_cpu(OAVideoTexture* self, FrameSlot* slot) {
    const int w    = slot->width;
    const int h    = slot->height;
    const int uv_w = (w + 1) / 2;
    const int uv_h = (h + 1) / 2;

    // Upload Y plane as R8
    glBindTexture(GL_TEXTURE_2D, self->y_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (self->y_tex_w != w || self->y_tex_h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, slot->y_plane.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        self->y_tex_w = w;
        self->y_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RED, GL_UNSIGNED_BYTE, slot->y_plane.data());
    }

    // Upload UV plane as RG8
    glBindTexture(GL_TEXTURE_2D, self->uv_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (self->uv_tex_w != uv_w || self->uv_tex_h != uv_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uv_w, uv_h, 0,
                     GL_RG, GL_UNSIGNED_BYTE, slot->uv_plane.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        self->uv_tex_w = uv_w;
        self->uv_tex_h = uv_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                        GL_RG, GL_UNSIGNED_BYTE, slot->uv_plane.data());
    }
    gl_check_errors("upload_nv12_cpu");
}

// ──────────────────────────────────────────────────────────────────────────────
// NV12 → RGBA FBO render
// ──────────────────────────────────────────────────────────────────────────────

static void render_nv12_to_rgba(OAVideoTexture* self, int w, int h) {
    // Ensure output RGBA texture is allocated
    if (!self->gl_tex) glGenTextures(1, &self->gl_tex);
    glBindTexture(GL_TEXTURE_2D, self->gl_tex);
    if (self->tex_w != w || self->tex_h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        self->tex_w = w;
        self->tex_h = h;
    }

    // Save GL state that we'll modify
    GLint prev_fbo = 0, prev_viewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    // Bind FBO with output texture as color attachment
    glBindFramebuffer(GL_FRAMEBUFFER, self->nv12_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, self->gl_tex, 0);

    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[OAVideoTexture] FBO INCOMPLETE: 0x"
                  << std::hex << fbo_status << std::dec
                  << " for " << w << "x" << h << "\n";
    }
    glViewport(0, 0, w, h);

    // Draw NV12 → RGB quad
    glUseProgram(self->nv12_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, self->y_tex);
    glUniform1i(self->loc_texY, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, self->uv_tex);
    glUniform1i(self->loc_texUV, 1);

    glBindBuffer(GL_ARRAY_BUFFER, self->nv12_vbo);
    glEnableVertexAttribArray(self->loc_aPos);
    glVertexAttribPointer(self->loc_aPos, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(self->loc_aTex);
    glVertexAttribPointer(self->loc_aTex, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_check_errors("render_nv12_to_rgba draw");

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1],
               prev_viewport[2], prev_viewport[3]);
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
    DropBuffer* buf = self->buf;

    auto emit_fallback = [&](const char* /*reason*/) {
        if (!self->gl_tex) glGenTextures(1, &self->gl_tex);
        static const uint8_t red[] = {0xFF, 0x00, 0x00, 0xFF};
        glBindTexture(GL_TEXTURE_2D, self->gl_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, red);
        *width = 1; *height = 1;
        *target = GL_TEXTURE_2D; *name = self->gl_tex;
    };

    if (!buf) {
        emit_fallback("no DropBuffer");
        return TRUE;
    }

    // 1. Release previous slot
    if (self->prev_slot) {
        std::lock_guard<std::mutex> lock(buf->mutex);
        buf->in_use = false;
        self->prev_slot = nullptr;
    }

    // 2. Swap in latest pending if available
    FrameSlot* next = buf->pending_slot.exchange(nullptr, std::memory_order_acquire);
    if (next) {
        std::lock_guard<std::mutex> lock(buf->mutex);
        buf->displaying_slot = next;
    }

    // 3. If nothing to display, return last known texture or fallback
    if (!buf->displaying_slot ||
        buf->displaying_slot->width <= 0 ||
        buf->displaying_slot->height <= 0 ||
        buf->displaying_slot->format == FrameFormat::kNone) {
        if (self->gl_tex && self->tex_w > 0 && self->tex_h > 0) {
            *target = GL_TEXTURE_2D;
            *name   = self->gl_tex;
            *width  = (uint32_t)self->tex_w;
            *height = (uint32_t)self->tex_h;
            return TRUE;
        }
        emit_fallback("no displaying_slot or bad dims/format");
        return TRUE;
    }

    FrameSlot* slot = buf->displaying_slot;

    // 4. Mark in_use, save for next tick
    {
        std::lock_guard<std::mutex> lock(buf->mutex);
        buf->in_use = true;
    }
    self->prev_slot = slot;

    const int w = slot->width;
    const int h = slot->height;

    switch (slot->format) {

    case FrameFormat::kNV12_DMABUF: {
        ensure_nv12_resources(self);
        if (!self->nv12_inited) { emit_fallback("NV12 shader init failed (dmabuf)"); return TRUE; }

        bool ok = upload_nv12_dmabuf(self, slot);
        if (ok) {
            render_nv12_to_rgba(self, w, h);
            if (!self->logged_path) {
                std::cout << "[OAVideoTexture] rendering NV12 DMA-BUF (zero-copy)\n";
                self->logged_path = true;
            }
        } else {
            std::cerr << "[OAVideoTexture] DMA-BUF EGL import failed for "
                      << w << "x" << h << "\n";
            buf->dmabuf_import_failed.store(true, std::memory_order_relaxed);
            // Can't fall back to CPU (data is in fds, not mapped).
            if (self->gl_tex && self->tex_w > 0) {
                *target = GL_TEXTURE_2D;
                *name   = self->gl_tex;
                *width  = (uint32_t)self->tex_w;
                *height = (uint32_t)self->tex_h;
                return TRUE;
            }
            emit_fallback("DMA-BUF import failed");
            return TRUE;
        }
        break;
    }

    case FrameFormat::kNV12_CPU: {
        ensure_nv12_resources(self);
        if (!self->nv12_inited) { emit_fallback("NV12 shader init failed"); return TRUE; }

        upload_nv12_cpu(self, slot);
        render_nv12_to_rgba(self, w, h);
        if (!self->logged_path) {
            std::cout << "[OAVideoTexture] rendering NV12 CPU (Y/UV texture upload)\n";
            self->logged_path = true;
        }
        break;
    }

    case FrameFormat::kRGBA: {
        if (!self->gl_tex) glGenTextures(1, &self->gl_tex);
        glBindTexture(GL_TEXTURE_2D, self->gl_tex);

        const size_t expected = static_cast<size_t>(w) * h * 4;
        if (slot->pixels.size() < expected) {
            emit_fallback("RGBA pixels too small");
            return TRUE;
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (self->tex_w != w || self->tex_h != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, slot->pixels.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            self->tex_w = w;
            self->tex_h = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_RGBA, GL_UNSIGNED_BYTE, slot->pixels.data());
        }
        if (!self->logged_path) {
            std::cout << "[OAVideoTexture] rendering RGBA CPU (direct upload)\n";
            self->logged_path = true;
        }
        break;
    }

    default:
        emit_fallback("unknown format");
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

    EGLDisplay dpy = eglGetCurrentDisplay();
    if (self->eglDestroyImageKHR_fn && dpy != EGL_NO_DISPLAY) {
        if (self->prev_y_img != EGL_NO_IMAGE_KHR) {
            self->eglDestroyImageKHR_fn(dpy, self->prev_y_img);
            self->prev_y_img = EGL_NO_IMAGE_KHR;
        }
        if (self->prev_uv_img != EGL_NO_IMAGE_KHR) {
            self->eglDestroyImageKHR_fn(dpy, self->prev_uv_img);
            self->prev_uv_img = EGL_NO_IMAGE_KHR;
        }
    }

    if (self->nv12_fbo)     { glDeleteFramebuffers(1, &self->nv12_fbo); self->nv12_fbo = 0; }
    if (self->nv12_vbo)     { glDeleteBuffers(1, &self->nv12_vbo); self->nv12_vbo = 0; }
    if (self->nv12_program) { glDeleteProgram(self->nv12_program); self->nv12_program = 0; }
    if (self->y_tex)        { glDeleteTextures(1, &self->y_tex); self->y_tex = 0; }
    if (self->uv_tex)       { glDeleteTextures(1, &self->uv_tex); self->uv_tex = 0; }
    if (self->gl_tex)       { glDeleteTextures(1, &self->gl_tex); self->gl_tex = 0; }

    self->tex_w = self->tex_h = 0;
    self->nv12_inited = false;

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

OAVideoTexture* oa_video_texture_new(DropBuffer* buf) {
    OAVideoTexture* self = OA_VIDEO_TEXTURE(
        g_object_new(OA_VIDEO_TEXTURE_TYPE, nullptr));
    self->buf = buf;
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

void oa_video_texture_mark_frame_available(OAVideoTexture* self,
                                           FlTextureRegistrar* registrar) {
    fl_texture_registrar_mark_texture_frame_available(registrar, FL_TEXTURE(self));
}
