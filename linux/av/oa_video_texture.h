// OAVideoTexture — Flutter GL texture fed from a DropBuffer.
//
// Handles three frame formats from the H264Decoder:
//   1. NV12 DMA-BUF  — zero-copy EGL import + NV12→RGB shader
//   2. NV12 CPU      — Y/UV texture upload + NV12→RGB shader
//   3. RGBA CPU      — direct glTexImage2D upload
//
// All GL work happens on Flutter's raster thread (populate callback).

#pragma once

#include "h264_decoder.h"

#include <flutter_linux/flutter_linux.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define OA_VIDEO_TEXTURE_TYPE (oa_video_texture_get_type())
G_DECLARE_FINAL_TYPE(OAVideoTexture, oa_video_texture, OA, VIDEO_TEXTURE, FlTextureGL)

/// Create a texture that reads frames from the given DropBuffer.
OAVideoTexture* oa_video_texture_new(DropBuffer* buf);

/// Register the texture with the Flutter engine and return its texture ID.
/// Call once; subsequent calls return the same ID.
int64_t oa_video_texture_register(OAVideoTexture* self, FlTextureRegistrar* registrar);

/// Notify Flutter that a new frame is available for this texture.
void oa_video_texture_mark_frame_available(OAVideoTexture* self,
                                           FlTextureRegistrar* registrar);

G_END_DECLS
