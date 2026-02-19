#pragma once


#include <flutter_linux/flutter_linux.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define OA_VIDEO_TEXTURE_TYPE (oa_video_texture_get_type())
G_DECLARE_FINAL_TYPE(OAVideoTexture, oa_video_texture, OA, VIDEO_TEXTURE, FlTextureGL)

// Create a texture placeholder of initial size (width x height).
// This texture supports YUV420P input and converts to RGBA in GL.
OAVideoTexture* oa_video_texture_new(int width, int height);

// Register the texture with the Flutter engine and return its texture ID.
// Call once; subsequent calls return the same ID.
int64_t oa_video_texture_register(OAVideoTexture* self, FlTextureRegistrar* registrar);

// Supply a YUV420P (I420) frame packed as [Y][U][V] with sizes
//   Y: width*height bytes
//   U: (width/2)*(height/2) bytes
//   V: (width/2)*(height/2) bytes
// The data is copied immediately; GL upload and YUV->RGBA occur in populate().
void oa_video_texture_set_yuv420p_frame(OAVideoTexture* self,
                                        const guint8* yuv_bytes,
                                        gsize length,
                                        int width,
                                        int height);

// Notify Flutter that a new frame is available for this texture.
void oa_video_texture_mark_frame_available(OAVideoTexture* self,
                                          FlTextureRegistrar* registrar);

G_END_DECLS
