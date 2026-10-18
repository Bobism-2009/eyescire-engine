/* C API for filament_bridge.dll. The engine is built with llvm-mingw, and
   Filament is an MSVC C++ library, so the two only meet through this DLL. */
#pragma once

#ifdef FILAMENT_BRIDGE_EXPORTS
#define FB_API __declspec(dllexport)
#else
#define FB_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the view is ready. hwnd is the child window Filament presents into.
   The window must not already have a pixel format. */
FB_API int fb_init(void* hwnd);
FB_API void fb_shutdown(void);
FB_API int fb_ok(void);
FB_API const char* fb_last_error(void);

/* Camera is world space, +Y up. up is the camera's up. fov is vertical degrees.
   fog_dist is world units from the camera to full fog; 0 leaves the view clear. */
FB_API void fb_begin(float ex, float ey, float ez,
                     float tx, float ty, float tz,
                     float ux, float uy, float uz,
                     float fov_deg, float aspect,
                     int w, int h,
                     int br, int bg, int bb,
                     int aa, int shadows,
                     int fog, int fog_r, int fog_g, int fog_b, float fog_dist);

/* dir is the direction the light travels. color is linear 0..1. */
FB_API void fb_sun(float dx, float dy, float dz, float r, float g, float b, float lux, int shadows);
FB_API void fb_point(float x, float y, float z, float r, float g, float b, float candela, float radius);
FB_API void fb_spot(float x, float y, float z, float dx, float dy, float dz,
                    float r, float g, float b, float candela, float radius,
                    float inner_rad, float outer_rad);
/* Linear irradiance-like color. Replaces the previous ambient. */
FB_API void fb_ambient(float r, float g, float b);

/* Six RGB faces, row 0 at the bottom, order +X -X +Y -Y +Z -Z. stamp skips a repeat upload. */
FB_API void fb_sky(const unsigned char* const faces[6], const int wh[12], const char* stamp);

/* kind: 1 box, 2 sphere, 3 cylinder. xform is column-major. color is sRGB 0..255.
   mode: 0 lit, 1 unlit depth-tested, 2 unlit drawn in front. */
FB_API void fb_prim(int kind, const float xform[16], int r, int g, int b, int a,
                    int cast_shadow, int mode);

/* Roughness/metallic in 0..1, bump strength (1 = normal). Call before mapped draws. */
FB_API void fb_mat_params(float roughness, float metallic, float bump, float tile, float triplanar);

/* Same as fb_prim, but with optional PBR maps (null skips). Maps are sampled
   with the mesh UVs into Filament's standard lit inputs. Height is vertex
   displacement done before this call, not a shader input. */
FB_API void fb_prim_maps(int kind, const float xform[16], int r, int g, int b, int a,
                         int cast_shadow, int mode,
                         const unsigned char* albedo, int aw, int ah,
                         const unsigned char* normal, int nw, int nh,
                         const unsigned char* metal, int mw, int mh,
                         const unsigned char* rough, int rw, int rh,
                         const unsigned char* height, int hw, int hh);

/* Unindexed triangle list. pos/nrm are tightly packed float3, count is vertices.
   uv may be null. albedo and the maps are RGB, row 0 at the bottom; null skips that map. */
FB_API void fb_model(const void* key,
                     const float* pos, const float* nrm, const float* uv, int count,
                     const float xform[16], int r, int g, int b, int a, int cast_shadow,
                     const unsigned char* albedo, int aw, int ah,
                     const unsigned char* normal, int nw, int nh,
                     const unsigned char* metal, int mw, int mh,
                     const unsigned char* rough, int rw, int rh,
                     const unsigned char* height, int hw, int hh);

/* Unindexed triangles, one flat color. lit=0 skips tangents. */
FB_API void fb_tris(const float* pos, const float* nrm, int count, const float xform[16],
                    int r, int g, int b, int a,
                    int lit, int depth_test, int depth_write, int cast_shadow);

FB_API void fb_line(float x1, float y1, float z1, float x2, float y2, float z2,
                    int r, int g, int b, int a, int depth_test);

/* One camera-facing quad. pos is 4 xyz, uv is 4 pairs. rgba is top-first. */
FB_API void fb_sprite(const void* key, const unsigned char* rgba, int tw, int th,
                      const float pos[12], const float uv[8],
                      int r, int g, int b, int a);

/* Device-space quad. x0,y0,x1,y1 are clip coordinates, y = +1 at the top. */
FB_API void fb_screen(float x0, float y0, float x1, float y1, int r, int g, int b, int a);

/* World point -> viewport pixels (y down, origin top-left of the view).
   Returns 1 when the point is in front of the camera. */
FB_API int fb_project(float wx, float wy, float wz, float* sx, float* sy);

/* Top-down BGRA, or null when the HUD is empty. */
FB_API void fb_overlay(const void* bgra, int w, int h);

/* Presents the frame. capture=1 also reads it back. Returns 1 if a frame was shown. */
FB_API int fb_draw(int capture);
/* Copies the last capture as top-down RGBA. Returns 1 on success. */
FB_API int fb_take_capture(unsigned char* dst, int bytes, int* w, int* h);

#ifdef __cplusplus
}
#endif
