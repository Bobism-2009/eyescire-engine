/* =============================================================================
 * Eyescire Engine - 3D view (included at the end of engine.cpp).
 *
 * The editor draws 3D scenes inside its own GDI window, where std/gfx3d's
 * window can't go. Frames are drawn off-screen, read into a DIB and blitted
 * onto the back buffer, so the editor's 2D chrome still goes on top. The GPU
 * draws them (OpenGL, multisampled; namespace glb); a small z-buffered
 * rasterizer is the fallback. Both follow EyescirePlayer3D's rules
 * (examples/player3d.nxa + engine/shape3d.nxa): world units are scene px / 32,
 * y is up (scene y flipped), z points out of the screen (scene z flipped),
 * boxes / spheres / upright cylinders / capsules / .obj models, flat colors lit by one
 * directional light.
 *
 *   engine_r3d_begin(...)    camera + viewport rect, clears the frame
 *   engine_r3d_entity(slot)  draws one entity
 *   engine_r3d_line(...)     depth-tested line in world units (grids, gizmos)
 *   engine_r3d_end(outline)  outlines selected entities, blits to the back buffer
 *   engine_r3d_pick(x, y)    entity under a back-buffer pixel (ray cast)
 *   engine_r3d_project / engine_r3d_plane_hit   world <-> screen helpers
 * ============================================================================= */

#include <GL/gl.h>
#include "../../Nexa-Lang/include/nexa_vk14.hpp"

namespace r3d {

constexpr float kUnit = 1.0f / 32.0f;
constexpr float kNear = 0.05f;

struct V3 { float x, y, z; };
static V3 v3(float x, float y, float z) { return V3{ x, y, z }; }
static V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V3 norm(V3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 1e-8f ? a * (1.0f / l) : v3(0, 0, 1);
}

struct Target {
    HDC      dc   = nullptr;
    HBITMAP  bmp  = nullptr;
    HGDIOBJ  old  = nullptr;
    uint32_t* px  = nullptr;   /* top-down BGRA */
    int      w = 0, h = 0;
    std::vector<float> invz;   /* 1/z of the nearest surface, 0 = empty */
    std::vector<int>   ids;    /* slot per pixel, -1 = background */
};
static Target T;

struct Cam {
    V3 eye, target, right, up, fwd;
    float f = 1.0f;            /* focal length in px */
    int sx = 0, sy = 0;        /* viewport origin on the back buffer */
    bool valid = false;
};
static Cam C;

/* 1 = OpenGL 3.3, which is what the view opens with. 0 is the 1.1 path.
   g_wantVk asks for Vulkan 1.4. If that device cannot be created, ready()
   opens OpenGL 3.3 and does not claim Vulkan. */
static int g_want33 = 1;
static int g_wantVk = 0;

static const V3 kDefaultLight = norm(v3(-0.45f, 1.0f, 0.75f));
static V3 g_light = kDefaultLight;     /* toward the sun (software path) */

/* ----- lights: see engine/shape3d.nxa for the rules ----- */
struct Light {
    int   type;              /* 0 directional, 1 point, 2 spot, 3 ambient */
    V3    pos, dir;          /* dir: the way it shines */
    float r, g, b;           /* color * intensity */
    float range;             /* world units */
    float cone;              /* full cone angle, degrees */
};

static int light_type(const std::string& k) {
    if (k == "light_directional") return 0;
    if (k == "light_point") return 1;
    if (k == "light_spot") return 2;
    if (k == "light_ambient") return 3;
    return -1;
}

static std::vector<Light> g_lights;    /* this frame's, from the scene */
static float g_px = 0, g_py = 0;       /* engine_r3d_project result */
static float g_hz = 0;                 /* engine_r3d_ray_point only */
static float g_hx = 0, g_hy = 0;       /* engine_r3d_plane_hit result (scene px) */

static void ensure_target(int w, int h) {
    if (T.bmp && T.w == w && T.h == h) return;
    if (T.dc) {
        SelectObject(T.dc, T.old);
        DeleteObject(T.bmp);
        DeleteDC(T.dc);
    }
    T = Target{};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return;
    T.dc = CreateCompatibleDC(nullptr);
    T.bmp = bmp;
    T.old = SelectObject(T.dc, bmp);
    T.px = (uint32_t*)bits;
    T.w = w;
    T.h = h;
    T.invz.assign((size_t)w * h, 0.0f);
    T.ids.assign((size_t)w * h, -1);
}

static uint32_t pack(int r, int g_, int b) {
    r = std::clamp(r, 0, 255); g_ = std::clamp(g_, 0, 255); b = std::clamp(b, 0, 255);
    return (uint32_t)((r << 16) | (g_ << 8) | b);
}

static int meshA = 255;
static int g_paintOver = 0;   /* gizmo cones paint on top and leave pick ids alone */

/* camera space: x right, y up, z = distance in front of the eye */
static V3 to_cam(V3 p) {
    V3 d = p - C.eye;
    return v3(dot(d, C.right), dot(d, C.up), dot(d, C.fwd));
}

struct SV { float x, y, iz, shade; };  /* screen vertex */

static SV to_screen(V3 c, float shade) {
    float iz = 1.0f / c.z;
    return SV{ T.w * 0.5f + c.x * C.f * iz, T.h * 0.5f - c.y * C.f * iz, iz, shade };
}

static void raster_tri(const SV& a, const SV& b, const SV& c, int cr, int cg, int cb, int id) {
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    int x0 = std::max(0, (int)std::floor(std::min({ a.x, b.x, c.x })));
    int x1 = std::min(T.w - 1, (int)std::ceil(std::max({ a.x, b.x, c.x })));
    int y0 = std::max(0, (int)std::floor(std::min({ a.y, b.y, c.y })));
    int y1 = std::min(T.h - 1, (int)std::ceil(std::max({ a.y, b.y, c.y })));
    if (x0 > x1 || y0 > y1) return;
    float inv = 1.0f / area;
    /* Edge weights are linear, so each pixel steps from the row start instead
       of recomputing the barycentric formula. Coverage matches the direct test. */
    const float dw0x = (b.y - c.y) * inv;
    const float dw1x = (c.y - a.y) * inv;
    const float dw2x = -dw0x - dw1x;
    for (int y = y0; y <= y1; ++y) {
        float py = y + 0.5f;
        float w0 = 0, w1 = 0, w2 = 0;
        size_t row = (size_t)y * T.w;
        for (int x = x0; x <= x1; ++x) {
            if (((x - x0) & 31) == 0) {
                float px_ = x + 0.5f;
                w0 = ((b.x - px_) * (c.y - py) - (b.y - py) * (c.x - px_)) * inv;
                w1 = ((c.x - px_) * (a.y - py) - (c.y - py) * (a.x - px_)) * inv;
                w2 = 1.0f - w0 - w1;
            }
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                float iz = w0 * a.iz + w1 * b.iz + w2 * c.iz;
                size_t i = row + x;
                if (g_paintOver || iz > T.invz[i]) {
                    if (!g_paintOver) {
                        T.invz[i] = iz;
                        T.ids[i] = id;
                    }
                    float s = w0 * a.shade + w1 * b.shade + w2 * c.shade;
                    int nr = (int)(cr * s);
                    int ng = (int)(cg * s);
                    int nb = (int)(cb * s);
                    if (meshA >= 255) {
                        T.px[i] = pack(nr, ng, nb);
                    } else {
                        uint32_t old = T.px[i];
                        int orr = (int)((old >> 16) & 255);
                        int og = (int)((old >> 8) & 255);
                        int ob = (int)(old & 255);
                        int al = meshA;
                        T.px[i] = pack(orr + (nr - orr) * al / 255,
                                       og + (ng - og) * al / 255,
                                       ob + (nb - ob) * al / 255);
                    }
                }
            }
            w0 += dw0x; w1 += dw1x; w2 += dw2x;
        }
    }
}

struct CV { V3 p; float shade; };      /* camera-space vertex */

/* Clip a camera-space triangle to z >= near, then rasterize the fan. */
static void draw_tri_cam(CV a, CV b, CV c, int cr, int cg, int cb, int id) {
    CV in[3] = { a, b, c };
    CV out[4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const CV& p = in[i];
        const CV& q = in[(i + 1) % 3];
        bool pin = p.p.z >= kNear, qin = q.p.z >= kNear;
        if (pin) out[n++] = p;
        if (pin != qin) {
            float t = (kNear - p.p.z) / (q.p.z - p.p.z);
            out[n++] = CV{ p.p + (q.p - p.p) * t, p.shade + (q.shade - p.shade) * t };
        }
    }
    if (n < 3) return;
    SV s[4];
    for (int i = 0; i < n; ++i) s[i] = to_screen(out[i].p, out[i].shade);
    raster_tri(s[0], s[1], s[2], cr, cg, cb, id);
    if (n == 4) raster_tri(s[0], s[2], s[3], cr, cg, cb, id);
}

/* Shade at a world point. A scene light is the same color the GPU uses.
   With no lights, the default sun and lamp are already in g_lights. */
static float shade_at(V3 p, V3 n) {
    float amb = 0.55f;
    bool any_amb = false;
    float sum = 0;
    for (const Light& l : g_lights) {
        if (l.type == 3) {
            if (!any_amb) { amb = 0; any_amb = true; }
            amb += (l.r + l.g + l.b) / 3.0f;
            continue;
        }
        float nd = 0, att = 1;
        if (l.type == 0) {
            nd = dot(n, l.dir * -1.0f);
        } else {
            V3 to = l.pos - p;
            float d2 = dot(to, to);
            float dist = std::sqrt(d2);
            if (dist > 1e-4f) nd = dot(n, to * (1.0f / dist));
            else nd = 1;
            float range = l.range > 1e-3f ? l.range : 1.0f;
            att = 1.0f / (1.0f + d2 / (range * range));
            if (l.type == 2 && dist > 1e-4f) {
                float s = dot(to * (-1.0f / dist), l.dir);
                float deg = std::clamp(l.cone * 0.5f, 1.0f, 90.0f);
                float cutoff = std::cos(deg * 3.14159265f / 180.0f);
                if (s < cutoff) att = 0;
                else att *= std::pow(std::max(s, 0.0f), 6.0f);
            }
        }
        if (nd < 0) nd = 0;
        sum += nd * att * (l.r + l.g + l.b) / 3.0f;
    }
    float s = amb + sum;
    if (any_amb) {
        float d = sum;
        if (d < 0) d = 0;
        if (d > 1) d = 1;
        float fill = amb;
        if (fill < 0) fill = 0;
        s = d + fill * (1.0f - d);
    }
    return s > 1.0f ? 1.0f : s;
}

/* One entity's placement: local mesh -> world.
   m is orient_matrix: Ry(rotY) * Rx(rotX) * Rz(-rotDeg). */
struct Xform {
    V3 center, scale;
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    V3 point(V3 l) const {
        V3 s = v3(l.x * scale.x, l.y * scale.y, l.z * scale.z);
        return center + v3(m[0] * s.x + m[4] * s.y + m[8] * s.z,
                            m[1] * s.x + m[5] * s.y + m[9] * s.z,
                            m[2] * s.x + m[6] * s.y + m[10] * s.z);
    }
    V3 normal(V3 n) const {
        /* inverse-transpose of the scale, then the rotation */
        V3 s = norm(v3(n.x / scale.x, n.y / scale.y, n.z / scale.z));
        return v3(m[0] * s.x + m[4] * s.y + m[8] * s.z,
                   m[1] * s.x + m[5] * s.y + m[9] * s.z,
                   m[2] * s.x + m[6] * s.y + m[10] * s.z);
    }
};

static void xform_orient(Xform& xf, const Entity& e) {
    ::orient_matrix(xf.m, e.rotX, e.rotY, e.rotDeg);
}

/* World-space triangle with per-vertex normals (flat faces pass the same one). */
static void draw_tri_world(V3 a, V3 b, V3 c, V3 na, V3 nb, V3 nc, int r, int g_, int b_, int id) {
    draw_tri_cam(CV{ to_cam(a), shade_at(a, na) }, CV{ to_cam(b), shade_at(b, nb) },
                 CV{ to_cam(c), shade_at(c, nc) }, r, g_, b_, id);
}

static void mesh_box(const Xform& x, int r, int g_, int b, int id) {
    /* unit cube, half extents 0.5 */
    static const float F[6][4][3] = {
        { { .5f,-.5f,-.5f},{ .5f, .5f,-.5f},{ .5f, .5f, .5f},{ .5f,-.5f, .5f} },  /* +x */
        { {-.5f,-.5f, .5f},{-.5f, .5f, .5f},{-.5f, .5f,-.5f},{-.5f,-.5f,-.5f} },  /* -x */
        { {-.5f, .5f,-.5f},{-.5f, .5f, .5f},{ .5f, .5f, .5f},{ .5f, .5f,-.5f} },  /* +y */
        { {-.5f,-.5f, .5f},{-.5f,-.5f,-.5f},{ .5f,-.5f,-.5f},{ .5f,-.5f, .5f} },  /* -y */
        { {-.5f,-.5f, .5f},{ .5f,-.5f, .5f},{ .5f, .5f, .5f},{-.5f, .5f, .5f} },  /* +z */
        { { .5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{-.5f, .5f,-.5f},{ .5f, .5f,-.5f} },  /* -z */
    };
    static const float N[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    for (int f = 0; f < 6; ++f) {
        V3 n = x.normal(v3(N[f][0], N[f][1], N[f][2]));
        V3 p[4];
        for (int k = 0; k < 4; ++k) p[k] = x.point(v3(F[f][k][0], F[f][k][1], F[f][k][2]));
        /* back faces can be skipped: boxes are closed */
        if (dot(n, C.eye - p[0]) <= 0) continue;
        draw_tri_world(p[0], p[1], p[2], n, n, n, r, g_, b, id);
        draw_tri_world(p[0], p[2], p[3], n, n, n, r, g_, b, id);
    }
}

static void mesh_sphere(const Xform& x, int r, int g_, int b, int id) {
    const int SEG = 20, RING = 12;
    auto pt = [](int i, int j) {
        float th = (float)j / RING * 3.14159265f;
        float ph = (float)i / SEG * 6.2831853f;
        return v3(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
    };
    for (int j = 0; j < RING; ++j) {
        for (int i = 0; i < SEG; ++i) {
            V3 a = pt(i, j), b2 = pt(i + 1, j), c = pt(i + 1, j + 1), d = pt(i, j + 1);
            V3 wa = x.point(a * 0.5f), wb = x.point(b2 * 0.5f), wc = x.point(c * 0.5f), wd = x.point(d * 0.5f);
            V3 na = x.normal(a), nb = x.normal(b2), nc = x.normal(c), nd = x.normal(d);
            if (j > 0)        draw_tri_world(wa, wb, wc, na, nb, nc, r, g_, b, id);
            if (j < RING - 1) draw_tri_world(wa, wc, wd, na, nc, nd, r, g_, b, id);
        }
    }
}

/* Upright capsule in the unit box. Width is the diameter, height is the full
   length including the round ends. Ends stay spheres under the (w, h, w)
   scale: the cap is an ellipsoid here that the scale turns into a ball.
   Shorter than it is wide, it is a sphere of diameter h. */
struct CapsuleLocal { float rx, ry, cy; };

static CapsuleLocal capsule_local(float w, float h) {
    CapsuleLocal c;
    if (w < 1.f) w = 1.f;
    if (h < 1.f) h = 1.f;
    if (h <= w) {
        c.rx = 0.5f * h / w;
        c.ry = 0.5f;
        c.cy = 0.f;
    } else {
        c.rx = 0.5f;
        c.ry = 0.5f * w / h;
        c.cy = 0.5f - c.ry;
    }
    return c;
}

/* One vertex: local position, and a normal that inverse-transpose(scale)
   turns into the round-cap direction. emit is called three times per triangle. */
template <class F>
static void capsule_emit(const CapsuleLocal& c, float sx, float sy, float sz, F emit) {
    const int SEG = 28, RINGS = 8;
    auto put = [&](int side, float a, float phi) {
        float st = std::sin(a), ct = std::cos(a);
        float sp = std::sin(phi), cp = std::cos(phi);
        float px = c.rx * st * cp;
        float py = (float)side * (c.cy + c.ry * ct);
        float pz = c.rx * st * sp;
        float dx = px * sx;
        float dy = (float)side * c.ry * ct * sy;
        float dz = pz * sz;
        emit(px, py, pz, dx * sx, dy * sy, dz * sz);
    };
    for (int side = 1; side >= -1; side -= 2) {
        for (int j = 0; j < RINGS; ++j) {
            float a0 = (float)j / RINGS * 1.5707963f;
            float a1 = (float)(j + 1) / RINGS * 1.5707963f;
            for (int i = 0; i < SEG; ++i) {
                float p0 = (float)i / SEG * 6.2831853f;
                float p1 = (float)(i + 1) / SEG * 6.2831853f;
                put(side, a0, p0); put(side, a0, p1); put(side, a1, p1);
                put(side, a0, p0); put(side, a1, p1); put(side, a1, p0);
            }
        }
    }
    if (c.cy <= 0.0001f) return;
    for (int i = 0; i < SEG; ++i) {
        float p0 = (float)i / SEG * 6.2831853f;
        float p1 = (float)(i + 1) / SEG * 6.2831853f;
        float c0 = std::cos(p0), s0 = std::sin(p0);
        float c1 = std::cos(p1), s1 = std::sin(p1);
        auto band = [&](float y, float cp, float sp) {
            float px = c.rx * cp, pz = c.rx * sp;
            emit(px, y, pz, px * sx * sx, 0.f, pz * sz * sz);
        };
        band(-c.cy, c0, s0); band(-c.cy, c1, s1); band(c.cy, c1, s1);
        band(-c.cy, c0, s0); band(c.cy, c1, s1); band(c.cy, c0, s0);
    }
}

static void mesh_cylinder(const Xform& x, int r, int g_, int b, int id) {
    /* upright along local y, radius 0.5, height 1 */
    const int SEG = 24;
    V3 top = x.point(v3(0, .5f, 0)), bot = x.point(v3(0, -.5f, 0));
    V3 nt = x.normal(v3(0, 1, 0)), nbm = x.normal(v3(0, -1, 0));
    for (int i = 0; i < SEG; ++i) {
        float a0 = (float)i / SEG * 6.2831853f, a1 = (float)(i + 1) / SEG * 6.2831853f;
        V3 d0 = v3(std::cos(a0), 0, std::sin(a0)), d1 = v3(std::cos(a1), 0, std::sin(a1));
        V3 t0 = x.point(v3(d0.x * .5f, .5f, d0.z * .5f)), t1 = x.point(v3(d1.x * .5f, .5f, d1.z * .5f));
        V3 b0 = x.point(v3(d0.x * .5f, -.5f, d0.z * .5f)), b1 = x.point(v3(d1.x * .5f, -.5f, d1.z * .5f));
        V3 n0 = x.normal(d0), n1 = x.normal(d1);
        draw_tri_world(b0, b1, t1, n0, n1, n1, r, g_, b, id);
        draw_tri_world(b0, t1, t0, n0, n1, n0, r, g_, b, id);
        draw_tri_world(top, t1, t0, nt, nt, nt, r, g_, b, id);
        draw_tri_world(bot, b0, b1, nbm, nbm, nbm, r, g_, b, id);
    }
}

static void mesh_capsule(const Xform& x, float w, float h, int r, int g_, int b, int id) {
    CapsuleLocal c = capsule_local(w, h);
    V3 tri[3], nrm[3];
    int n = 0;
    capsule_emit(c, x.scale.x, x.scale.y, x.scale.z, [&](float px, float py, float pz, float nx, float ny, float nz) {
        tri[n] = x.point(v3(px, py, pz));
        nrm[n] = x.normal(v3(nx, ny, nz));
        if (++n == 3) {
            draw_tri_world(tri[0], tri[1], tri[2], nrm[0], nrm[1], nrm[2], r, g_, b, id);
            n = 0;
        }
    });
}

/* ----- .obj models: positions + faces, centered and scaled to fit a unit cube ----- */

struct MapImg {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb; /* row 0 is the bottom, same as an OBJ v of 0 */
};

struct Model {
    std::vector<V3>  pos;
    std::vector<int> tri;      /* 3 indices per triangle */
    std::vector<V3>  cn;       /* one normal per corner, same order as tri */
    std::vector<float> cuv;    /* u,v per corner */
    MapImg base, nrm, metal, rough, height;
    bool ok = false;
};
static std::unordered_map<std::string, Model> g_models;

/* Joining the project root allocates. The same model path is resolved on
   every draw, so keep the absolute path after the first lookup. */
static const std::string& model_path(const std::string& rel) {
    static std::unordered_map<std::string, std::string> cache;
    static std::string root;
    if (root != g.projRoot) {
        cache.clear();
        root = g.projRoot;
    }
    auto it = cache.find(rel);
    if (it != cache.end()) return it->second;
    return cache.emplace(rel, asset_resolve(rel)).first->second;
}

static std::string model_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static std::string model_dir(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

static std::unordered_map<std::string, MapImg> g_imgCache;

/* Uncompressed true-color TGA. The flashlight maps are 24-bit TGAs, which
   GDI+ does not open. */
static bool model_load_tga(const std::string& path, MapImg& im) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char hdr[18] = {};
    in.read((char*)hdr, 18);
    if (!in || hdr[2] != 2) return false;
    int w = hdr[12] | (hdr[13] << 8);
    int h = hdr[14] | (hdr[15] << 8);
    int bpp = hdr[16];
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return false;
    if (bpp != 24 && bpp != 32) return false;
    if (hdr[0]) in.seekg(hdr[0], std::ios::cur);
    int ch = bpp / 8;
    std::vector<unsigned char> src((size_t)w * h * ch);
    in.read((char*)src.data(), (std::streamsize)src.size());
    if (in.gcount() != (std::streamsize)src.size()) return false;
    bool top = (hdr[17] & 0x20) != 0;
    int side = w > h ? w : h;
    int tw = w, th = h;
    if (side > 2048) {
        float s = 2048.f / (float)side;
        tw = std::max(1, (int)(w * s));
        th = std::max(1, (int)(h * s));
    }
    im.w = tw;
    im.h = th;
    im.rgb.assign((size_t)tw * th * 3, 0);
    for (int j = 0; j < th; ++j) {
        int fromBottom = (int)((j + 0.5f) * h / th);
        if (fromBottom >= h) fromBottom = h - 1;
        int fromTop = h - 1 - fromBottom;
        int fileRow = top ? fromTop : fromBottom;
        const unsigned char* row = src.data() + (size_t)fileRow * w * ch;
        for (int i = 0; i < tw; ++i) {
            int si = (int)((i + 0.5f) * w / tw);
            if (si >= w) si = w - 1;
            const unsigned char* px = row + si * ch;
            size_t o = ((size_t)j * tw + i) * 3;
            im.rgb[o] = px[2];
            im.rgb[o + 1] = px[1];
            im.rgb[o + 2] = px[0];
        }
    }
    return true;
}

static bool model_load_image(const std::string& path, MapImg& im) {
    if (path.empty()) return false;
    auto cached = g_imgCache.find(path);
    if (cached != g_imgCache.end()) {
        im = cached->second;
        return im.w > 0;
    }
    if (path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (ext == ".tga" && model_load_tga(path, im)) {
            g_imgCache[path] = im;
            return im.w > 0;
        }
    }
    int n = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    Gdiplus::Bitmap src(w.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok || src.GetWidth() < 1 || src.GetHeight() < 1) return false;
    int sw = (int)src.GetWidth(), sh = (int)src.GetHeight();
    int side = sw > sh ? sw : sh;
    int tw = sw, th = sh;
    if (side > 2048) {
        float s = 2048.f / (float)side;
        tw = std::max(1, (int)(sw * s));
        th = std::max(1, (int)(sh * s));
    }
    Gdiplus::Bitmap bmp(tw, th, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    Gdiplus::Graphics gfx(&bmp);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    gfx.DrawImage(&src, 0, 0, tw, th);
    Gdiplus::Rect rc(0, 0, tw, th);
    Gdiplus::BitmapData bd{};
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;
    im.w = tw;
    im.h = th;
    im.rgb.assign((size_t)tw * th * 3, 0);
    for (int j = 0; j < th; ++j) {
        int srcRow = th - 1 - j;
        const unsigned char* row = (const unsigned char*)bd.Scan0 + srcRow * bd.Stride;
        for (int i = 0; i < tw; ++i) {
            size_t p = ((size_t)j * tw + i) * 3;
            im.rgb[p] = row[i * 4 + 2];
            im.rgb[p + 1] = row[i * 4 + 1];
            im.rgb[p + 2] = row[i * 4];
        }
    }
    bmp.UnlockBits(&bd);
    g_imgCache[path] = im;
    return true;
}

static void model_sample(const MapImg& im, float u, float v, int& r, int& g, int& b) {
    if (im.w < 1 || im.h < 1 || im.rgb.empty()) return;
    u -= std::floor(u);
    v -= std::floor(v);
    int x = (int)(u * (float)(im.w - 1));
    int y = (int)(v * (float)(im.h - 1));
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= im.w) x = im.w - 1;
    if (y >= im.h) y = im.h - 1;
    size_t p = ((size_t)y * im.w + x) * 3;
    r = im.rgb[p];
    g = im.rgb[p + 1];
    b = im.rgb[p + 2];
}

static std::string model_last_token(const std::string& line) {
    std::istringstream ss(line);
    std::string tok, last;
    while (ss >> tok) if (tok[0] != '-') last = tok;
    return last;
}

static MapImg model_gray_channel(const MapImg& src, int ch) {
    MapImg o;
    o.w = src.w;
    o.h = src.h;
    o.rgb.assign(src.rgb.size(), 0);
    if (ch < 0 || ch > 2) return o;
    for (size_t i = 0; i + 2 < src.rgb.size(); i += 3) {
        unsigned char v = src.rgb[i + (size_t)ch];
        o.rgb[i] = v;
        o.rgb[i + 1] = v;
        o.rgb[i + 2] = v;
    }
    return o;
}

/* Occlusion in the red channel. Kept off pure black so creases stay visible. */
static void model_bake_ao(MapImg& base, const MapImg& orm) {
    if (base.w < 1 || orm.w < 1 || base.rgb.empty() || orm.rgb.empty()) return;
    for (int j = 0; j < base.h; ++j) {
        int aj = (int)((j + 0.5f) * orm.h / base.h);
        if (aj >= orm.h) aj = orm.h - 1;
        if (aj < 0) aj = 0;
        for (int i = 0; i < base.w; ++i) {
            int ai = (int)((i + 0.5f) * orm.w / base.w);
            if (ai >= orm.w) ai = orm.w - 1;
            if (ai < 0) ai = 0;
            float a = 0.45f + 0.55f * (orm.rgb[((size_t)aj * orm.w + ai) * 3] / 255.0f);
            size_t o = ((size_t)j * base.w + i) * 3;
            base.rgb[o] = (unsigned char)std::clamp(base.rgb[o] * a, 0.0f, 255.0f);
            base.rgb[o + 1] = (unsigned char)std::clamp(base.rgb[o + 1] * a, 0.0f, 255.0f);
            base.rgb[o + 2] = (unsigned char)std::clamp(base.rgb[o + 2] * a, 0.0f, 255.0f);
        }
    }
}

/* Unreal exports DirectX normals. OpenGL wants the green channel the other way. */
static void model_flip_green(MapImg& im) {
    for (size_t i = 1; i + 2 < im.rgb.size(); i += 3)
        im.rgb[i] = (unsigned char)(255 - im.rgb[i]);
}

static void model_bind_maps(Model& m, const std::string& objPath, const std::string& mtllib) {
    std::string dir = model_dir(objPath);
    std::string base, nrm, metal, rough, height;
    auto slot_file = [&](std::string& slot, const std::string& rel) {
        if (!slot.empty() || rel.empty()) return;
        std::filesystem::path p = std::filesystem::path(dir) / rel;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) slot = p.string();
    };
    if (!mtllib.empty()) {
        std::filesystem::path mp = std::filesystem::path(dir) / mtllib;
        std::ifstream mf(mp.string());
        std::string line;
        while (std::getline(mf, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string low = model_lower(line);
            auto take = [&](std::string& slot, const char* key) {
                if (slot.empty() && low.rfind(key, 0) == 0)
                    slot_file(slot, model_last_token(line.substr(std::strlen(key))));
            };
            take(base, "map_kd");
            take(base, "map_ka");
            take(nrm, "map_kn");
            take(nrm, "map_bump");
            take(nrm, "bump");
            take(nrm, "norm");
            take(metal, "map_pm");
            take(metal, "map_metallic");
            take(rough, "map_pr");
            take(rough, "map_roughness");
            take(height, "map_disp");
            take(height, "disp");
            take(height, "map_height");
        }
    }
    std::string packed;
    std::string nrmPath;
    std::string ao;
    std::string stem = model_lower(std::filesystem::path(objPath).stem().string());
    auto rank = [&](const std::string& path) {
        std::string n = model_lower(std::filesystem::path(path).stem().string());
        if (n == stem || n.rfind(stem + "_", 0) == 0) return 2;
        return 1;
    };
    std::vector<std::filesystem::path> files;
    auto gather = [&](const std::filesystem::path& d) {
        std::error_code ec;
        if (!std::filesystem::is_directory(d, ec)) return;
        for (const auto& ent : std::filesystem::directory_iterator(d, ec)) {
            if (ec) break;
            if (ent.is_regular_file()) files.push_back(ent.path());
        }
    };
    std::filesystem::path folder(dir);
    gather(folder);
    gather(folder / "textures");
    gather(folder / "texture");
    for (const auto& f : files) {
        std::string ext = model_lower(f.extension().string());
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" && ext != ".tga") continue;
        std::string name = model_lower(f.stem().string());
        auto take = [&](std::string& slot, bool match) {
            if (!match) return;
            if (slot.empty() || rank(f.string()) > rank(slot)) slot = f.string();
        };
        /* OcclusionRoughnessMetallic is one picture: red occlusion, green
           roughness, blue metal. It is split below instead of sampled as red. */
        bool orm = name.find("occlusion") != std::string::npos
                && name.find("rough") != std::string::npos;
        if (orm && (packed.empty() || rank(f.string()) > rank(packed))) packed = f.string();
        bool aoMap = !orm && (name.find("ambientocclusion") != std::string::npos
                           || name.find("occlusion") != std::string::npos);
        if (aoMap && (ao.empty() || rank(f.string()) > rank(ao))) ao = f.string();
        take(rough, !orm && name.find("rough") != std::string::npos);
        take(metal, !orm && name.find("metal") != std::string::npos);
        take(height, name.find("height") != std::string::npos || name.find("displac") != std::string::npos);
        if (nrm.empty() && (name.find("normal") != std::string::npos || name.find("nrm") != std::string::npos))
            nrmPath = f.string();
        take(nrm, name.find("normal") != std::string::npos || name.find("nrm") != std::string::npos);
        take(base, name.find("basecolor") != std::string::npos || name.find("base_color") != std::string::npos
                   || name.find("albedo") != std::string::npos || name.find("diffuse") != std::string::npos);
    }
    model_load_image(base, m.base);
    model_load_image(nrm, m.nrm);
    model_load_image(metal, m.metal);
    model_load_image(rough, m.rough);
    model_load_image(height, m.height);
    if (!packed.empty()) {
        MapImg orm;
        if (model_load_image(packed, orm)) {
            model_bake_ao(m.base, orm);
            m.rough = model_gray_channel(orm, 1);
            m.metal = model_gray_channel(orm, 2);
        }
    } else if (!ao.empty()) {
        MapImg occ;
        if (model_load_image(ao, occ)) model_bake_ao(m.base, occ);
    }
    if (nrmPath.find("+-+") != std::string::npos) model_flip_green(m.nrm);
}

/* A face whose texture window covers a big slice of the picture pulls in
   whatever sits next to its chart. On a packed atlas that is the floor
   checker and the empty gap, which is the black opening at the end of a
   hall. Those faces are retiled from the wood chart the other walls use. */
struct WideBox {
    int x0, y0, z0, x1, y1, z1;
    bool operator==(const WideBox& o) const {
        return x0 == o.x0 && y0 == o.y0 && z0 == o.z0 && x1 == o.x1 && y1 == o.y1 && z1 == o.z1;
    }
};
struct WideBoxHash {
    size_t operator()(const WideBox& k) const {
        size_t h = (size_t)k.x0 * 73856093u ^ (size_t)k.y0 * 19349663u ^ (size_t)k.z0 * 83492791u;
        h ^= (size_t)k.x1 * 73856093u ^ (size_t)k.y1 * 19349663u ^ (size_t)k.z1 * 83492791u;
        return h;
    }
};

static void model_fix_wide_uvs(Model& m) {
    const int ntri = (int)m.tri.size() / 3;
    if (ntri < 1 || (int)m.cuv.size() < ntri * 6 || m.pos.empty()) return;
    struct Chart {
        int n = 0;
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        double scaleSum = 0;
        int scaleN = 0;
    };
    std::unordered_map<int, Chart> bins;
    std::vector<char> bad((size_t)ntri, 0);
    int badN = 0;
    for (int t = 0; t < ntri; ++t) {
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        for (int c = 0; c < 3; ++c) {
            float u = m.cuv[(size_t)(t * 3 + c) * 2];
            float v = m.cuv[(size_t)(t * 3 + c) * 2 + 1];
            u0 = std::min(u0, u); u1 = std::max(u1, u);
            v0 = std::min(v0, v); v1 = std::max(v1, v);
        }
        float du = u1 - u0, dv = v1 - v0;
        /* A face stretched across most of the atlas smears the ceiling and
           the join in the middle of the hall. Both-wide faces are the end
           walls; one long span is the cornice. */
        if ((du > 0.15f && dv > 0.15f) || du > 0.40f || dv > 0.40f) {
            bad[(size_t)t] = 1;
            ++badN;
            continue;
        }
        if (du > 0.08f || dv > 0.08f || du < 1e-5f || dv < 1e-5f) continue;
        int ia = m.tri[t * 3], ib = m.tri[t * 3 + 1], ic = m.tri[t * 3 + 2];
        if (ia < 0 || ib < 0 || ic < 0) continue;
        if (ia >= (int)m.pos.size() || ib >= (int)m.pos.size() || ic >= (int)m.pos.size()) continue;
        V3 a = m.pos[ia], b = m.pos[ib], c = m.pos[ic];
        V3 nrm = cross(b - a, c - a);
        float nl = std::sqrt(dot(nrm, nrm));
        if (nl < 1e-8f || std::fabs(nrm.y / nl) > 0.45f) continue;
        int key = (int)std::floor(((u0 + u1) * 0.5f) * 20.f) * 1000
                + (int)std::floor(((v0 + v1) * 0.5f) * 20.f);
        Chart& ch = bins[key];
        ch.n++;
        ch.u0 = std::min(ch.u0, u0); ch.u1 = std::max(ch.u1, u1);
        ch.v0 = std::min(ch.v0, v0); ch.v1 = std::max(ch.v1, v1);
        float edge = std::sqrt(dot(b - a, b - a));
        float ue = m.cuv[(size_t)(t * 3) * 2] - m.cuv[(size_t)(t * 3 + 1) * 2];
        float ve = m.cuv[(size_t)(t * 3) * 2 + 1] - m.cuv[(size_t)(t * 3 + 1) * 2 + 1];
        float duv = std::sqrt(ue * ue + ve * ve);
        if (duv > 1e-5f) { ch.scaleSum += edge / duv; ch.scaleN++; }
    }
    if (!badN || bins.empty()) return;
    int bestKey = 0, bestAny = 0, bestN = -1, bestAnyN = -1;
    for (const auto& kv : bins) {
        if (kv.second.n > bestAnyN) { bestAnyN = kv.second.n; bestAny = kv.first; }
        if (kv.first / 1000 >= 8 && kv.second.n > bestN) { bestN = kv.second.n; bestKey = kv.first; }
    }
    if (bestN < 0) bestKey = bestAny;
    Chart chart = bins[bestKey];
    for (int step = 1; step <= 4; ++step) {
        auto it = bins.find(bestKey + step * 1000);
        if (it == bins.end() || it->second.n * 5 < chart.n) break;
        chart.u0 = std::min(chart.u0, it->second.u0);
        chart.u1 = std::max(chart.u1, it->second.u1);
        chart.v0 = std::min(chart.v0, it->second.v0);
        chart.v1 = std::max(chart.v1, it->second.v1);
    }
    if (chart.scaleN < 1) return;
    if (chart.u1 - chart.u0 > 0.30f) chart.u1 = chart.u0 + 0.30f;
    if (chart.v1 - chart.v0 > 0.30f) chart.v1 = chart.v0 + 0.30f;
    if (chart.u1 - chart.u0 < 1e-4f || chart.v1 - chart.v0 < 1e-4f) return;
    float per = (float)(chart.scaleSum / chart.scaleN);
    float tileU = (chart.u1 - chart.u0) * per;
    float tileV = (chart.v1 - chart.v0) * per;
    if (tileU < 1e-6f || tileV < 1e-6f) return;

    auto box_of = [&](int t) {
        V3 a = m.pos[m.tri[t * 3]], b = m.pos[m.tri[t * 3 + 1]], c = m.pos[m.tri[t * 3 + 2]];
        WideBox k;
        k.x0 = (int)std::lround(std::min(a.x, std::min(b.x, c.x)) * 10000.f);
        k.y0 = (int)std::lround(std::min(a.y, std::min(b.y, c.y)) * 10000.f);
        k.z0 = (int)std::lround(std::min(a.z, std::min(b.z, c.z)) * 10000.f);
        k.x1 = (int)std::lround(std::max(a.x, std::max(b.x, c.x)) * 10000.f);
        k.y1 = (int)std::lround(std::max(a.y, std::max(b.y, c.y)) * 10000.f);
        k.z1 = (int)std::lround(std::max(a.z, std::max(b.z, c.z)) * 10000.f);
        return k;
    };
    std::unordered_map<WideBox, int, WideBoxHash> boxCount;
    for (int t = 0; t < ntri; ++t) if (bad[(size_t)t]) boxCount[box_of(t)]++;

    std::vector<int> tri;
    std::vector<V3> cn;
    std::vector<float> cuv;
    tri.reserve(m.tri.size() + (size_t)badN * 24);
    cn.reserve(m.cn.size() + (size_t)badN * 24);
    cuv.reserve(m.cuv.size() + (size_t)badN * 48);
    std::unordered_map<WideBox, int, WideBoxHash> filled;
    auto push_corner = [&](int index, V3 nrm, float u, float v) {
        tri.push_back(index);
        cn.push_back(nrm);
        cuv.push_back(u);
        cuv.push_back(v);
    };
    for (int t = 0; t < ntri; ++t) {
        if (!bad[(size_t)t]) {
            for (int c = 0; c < 3; ++c) {
                V3 nrm = (t * 3 + c < (int)m.cn.size()) ? m.cn[(size_t)(t * 3 + c)] : v3(0, 1, 0);
                push_corner(m.tri[t * 3 + c], nrm,
                            m.cuv[(size_t)(t * 3 + c) * 2], m.cuv[(size_t)(t * 3 + c) * 2 + 1]);
            }
            continue;
        }
        WideBox bk = box_of(t);
        if (filled[bk]) continue;
        filled[bk] = 1;
        V3 a = m.pos[m.tri[t * 3]], b = m.pos[m.tri[t * 3 + 1]], c = m.pos[m.tri[t * 3 + 2]];
        float minx = std::min(a.x, std::min(b.x, c.x)), maxx = std::max(a.x, std::max(b.x, c.x));
        float miny = std::min(a.y, std::min(b.y, c.y)), maxy = std::max(a.y, std::max(b.y, c.y));
        float minz = std::min(a.z, std::min(b.z, c.z)), maxz = std::max(a.z, std::max(b.z, c.z));
        float ex = maxx - minx, ey = maxy - miny, ez = maxz - minz;
        int axis = 0;
        if (ey <= ex && ey <= ez) axis = 1;
        else if (ez <= ex && ez <= ey) axis = 2;
        float s0, s1, t0, t1;
        if (axis == 0) { s0 = minz; s1 = maxz; t0 = miny; t1 = maxy; }
        else if (axis == 1) { s0 = minx; s1 = maxx; t0 = minz; t1 = maxz; }
        else { s0 = minx; s1 = maxx; t0 = miny; t1 = maxy; }
        if (s1 - s0 < 1e-6f || t1 - t0 < 1e-6f) continue;
        V3 faceN = norm(cross(b - a, c - a));
        float boxArea = (axis == 0 ? ez * ey : axis == 1 ? ex * ez : ex * ey);
        float triArea = std::sqrt(dot(cross(b - a, c - a), cross(b - a, c - a))) * 0.5f;
        bool grid = boxCount[bk] >= 2 && triArea > boxArea * 0.35f;
        auto at = [&](float s, float tv) {
            if (axis == 0) return v3((minx + maxx) * 0.5f, tv, s);
            if (axis == 1) return v3(s, (miny + maxy) * 0.5f, tv);
            return v3(s, tv, (minz + maxz) * 0.5f);
        };
        auto emit = [&](V3 p, V3 q, V3 r, float pu, float pv, float qu, float qv, float ru, float rv) {
            if (dot(cross(q - p, r - p), faceN) < 0) {
                std::swap(q, r);
                std::swap(qu, ru);
                std::swap(qv, rv);
            }
            int base = (int)m.pos.size();
            m.pos.push_back(p);
            m.pos.push_back(q);
            m.pos.push_back(r);
            push_corner(base, faceN, pu, pv);
            push_corner(base + 1, faceN, qu, qv);
            push_corner(base + 2, faceN, ru, rv);
        };
        if (!grid) {
            auto uv_of = [&](V3 p, float& u, float& v) {
                float s, tv;
                if (axis == 0) { s = p.z; tv = p.y; }
                else if (axis == 1) { s = p.x; tv = p.z; }
                else { s = p.x; tv = p.y; }
                u = chart.u0 + (s - s0) / (s1 - s0) * (chart.u1 - chart.u0);
                v = chart.v0 + (tv - t0) / (t1 - t0) * (chart.v1 - chart.v0);
            };
            float ua, va, ub, vb, uc, vc;
            uv_of(a, ua, va); uv_of(b, ub, vb); uv_of(c, uc, vc);
            emit(a, b, c, ua, va, ub, vb, uc, vc);
            continue;
        }
        int ns = std::max(1, (int)std::ceil((s1 - s0) / tileU));
        int nt = std::max(1, (int)std::ceil((t1 - t0) / tileV));
        if (ns > 32) ns = 32;
        if (nt > 32) nt = 32;
        float stepS = (s1 - s0) / (float)ns;
        float stepT = (t1 - t0) / (float)nt;
        for (int i = 0; i < ns; ++i) {
            for (int j = 0; j < nt; ++j) {
                float sa = s0 + stepS * (float)i, sb = sa + stepS;
                float ta = t0 + stepT * (float)j, tb = ta + stepT;
                emit(at(sa, ta), at(sb, ta), at(sb, tb), chart.u0, chart.v0, chart.u1, chart.v0, chart.u1, chart.v1);
                emit(at(sa, ta), at(sb, tb), at(sa, tb), chart.u0, chart.v0, chart.u1, chart.v1, chart.u0, chart.v1);
            }
        }
    }
    m.tri.swap(tri);
    m.cn.swap(cn);
    m.cuv.swap(cuv);
    engine_log("[model] repaired wide unwrap");
}

static const Model& model_load(const std::string& path) {
    auto it = g_models.find(path);
    if (it != g_models.end()) return it->second;
    Model& m = g_models[path];
    std::ifstream f(path);
    if (!f) return m;
    std::vector<V3> raw, rawN;
    std::vector<float> rawUv;
    std::string mtllib;
    std::string line;
    auto fix = [](int idx, int n) {
        if (idx < 0) return n + idx;
        return idx - 1;
    };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 7 && model_lower(line.substr(0, 7)) == "mtllib ") {
            mtllib = model_last_token(line);
        } else if (line.size() > 2 && line[0] == 'v' && line[1] == ' ') {
            V3 p{};
            if (sscanf(line.c_str() + 2, "%f %f %f", &p.x, &p.y, &p.z) == 3) raw.push_back(p);
        } else if (line.size() > 3 && line[0] == 'v' && line[1] == 't' && line[2] == ' ') {
            float u = 0, v = 0;
            if (sscanf(line.c_str() + 3, "%f %f", &u, &v) >= 1) {
                rawUv.push_back(u);
                rawUv.push_back(v);
            }
        } else if (line.size() > 3 && line[0] == 'v' && line[1] == 'n' && line[2] == ' ') {
            V3 p{};
            if (sscanf(line.c_str() + 3, "%f %f %f", &p.x, &p.y, &p.z) == 3) rawN.push_back(p);
        } else if (line.size() > 2 && line[0] == 'f' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            std::string tok;
            std::vector<int> face, fuv, fn;
            while (ss >> tok) {
                int vi = 0, ti = 0, ni = 0;
                int slashes = 0;
                for (char c : tok) if (c == '/') ++slashes;
                bool okTok = false;
                if (tok.find("//") != std::string::npos) {
                    okTok = sscanf(tok.c_str(), "%d//%d", &vi, &ni) == 2;
                    ti = 0;
                } else if (slashes >= 2) {
                    okTok = sscanf(tok.c_str(), "%d/%d/%d", &vi, &ti, &ni) == 3;
                } else if (slashes == 1) {
                    okTok = sscanf(tok.c_str(), "%d/%d", &vi, &ti) == 2;
                } else {
                    okTok = sscanf(tok.c_str(), "%d", &vi) == 1;
                }
                if (!okTok || vi == 0) continue;
                face.push_back(fix(vi, (int)raw.size()));
                fuv.push_back(ti == 0 ? -1 : fix(ti, (int)rawUv.size() / 2));
                fn.push_back(ni == 0 ? -1 : fix(ni, (int)rawN.size()));
            }
            for (size_t k = 1; k + 1 < face.size(); ++k) {
                int corner[3] = { 0, (int)k, (int)k + 1 };
                for (int c = 0; c < 3; ++c) {
                    int vi = face[corner[c]];
                    if (vi < 0 || vi >= (int)raw.size()) vi = 0;
                    m.tri.push_back(vi);
                    int ti = fuv[corner[c]];
                    if (ti >= 0 && (ti + 1) * 2 <= (int)rawUv.size()) {
                        m.cuv.push_back(rawUv[ti * 2]);
                        m.cuv.push_back(rawUv[ti * 2 + 1]);
                    } else {
                        m.cuv.push_back(0);
                        m.cuv.push_back(0);
                    }
                    int ni = fn[corner[c]];
                    if (ni >= 0 && ni < (int)rawN.size()) m.cn.push_back(rawN[ni]);
                    else m.cn.push_back(v3(0, 0, 0));
                }
            }
        }
    }
    if (raw.empty()) return m;
    V3 lo = raw[0], hi = raw[0];
    for (const V3& p : raw) {
        lo = v3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = v3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    V3 mid = (lo + hi) * 0.5f;
    float ext = std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z });
    if (ext <= 0) ext = 1;
    for (const V3& p : raw) m.pos.push_back((p - mid) * (1.0f / ext));
    for (int& i : m.tri) if (i < 0 || i >= (int)m.pos.size()) i = 0;
    /* That retile is for the hallway atlas. On any other mesh it pulls
       real islands apart, including the flashlight body. */
    if (model_lower(path).find("horror corridor") != std::string::npos)
        model_fix_wide_uvs(m);
    m.ok = !m.tri.empty();
    if (m.ok) {
        model_bind_maps(m, path, mtllib);
        if (m.base.w || m.nrm.w || m.metal.w || m.rough.w || m.height.w) {
            std::string note = "[model] maps";
            if (m.base.w) note += " base";
            if (m.nrm.w) note += " normal";
            if (m.metal.w) note += " metallic";
            if (m.rough.w) note += " roughness";
            if (m.height.w) note += " height";
            engine_log(note.c_str());
        }
    }
    return m;
}

static void mesh_model(const Model& m, const Xform& x, int r, int g_, int b, int id) {
    for (size_t k = 0; k + 2 < m.tri.size(); k += 3) {
        V3 a = x.point(m.pos[m.tri[k]]), b2 = x.point(m.pos[m.tri[k + 1]]), c = x.point(m.pos[m.tri[k + 2]]);
        V3 n = norm(cross(b2 - a, c - a));
        /* models may be open or wound either way: light the side facing us */
        if (dot(n, C.eye - a) < 0) n = n * -1.0f;
        int tr = r, tg = g_, tb = b;
        if (m.base.w > 0 && (k + 2) * 2 + 1 < m.cuv.size()) {
            float u = (m.cuv[k * 2] + m.cuv[(k + 1) * 2] + m.cuv[(k + 2) * 2]) / 3.f;
            float v = (m.cuv[k * 2 + 1] + m.cuv[(k + 1) * 2 + 1] + m.cuv[(k + 2) * 2 + 1]) / 3.f;
            model_sample(m.base, u, v, tr, tg, tb);
        }
        draw_tri_world(a, b2, c, n, n, n, tr, tg, tb, id);
    }
}

static int depth_of(const Entity& e) {
    if (e.depth > 0) return e.depth;
    if (e.kind == "cube" || e.kind == "cylinder") return e.w;
    if (e.kind == "model") return std::max(e.w, e.h);
    return std::min(e.w, e.h);
}


static void collect_lights() {
    g_lights.clear();
    g_light = kDefaultLight;
    bool sun = false;
    for (const Entity& e : g.entities) {
        if (!e.active) continue;
        if (!entity_shown((int)(&e - g.entities.data()))) continue;
        int type = light_type(e.kind);
        if (type < 0) continue;
        Light l{};
        l.type = type;
        l.pos = v3((e.x + e.w * 0.5f) * kUnit, -(e.y + e.h * 0.5f) * kUnit, -e.z * kUnit);
        float rr = (float)e.rotDeg * 3.14159265f / 180.0f, t = (float)e.lightTilt * 3.14159265f / 180.0f;
        l.dir = v3(-std::sin(rr) * std::cos(t), -std::cos(rr) * std::cos(t), -std::sin(t));
        float k = e.lightIntensity / 100.0f / 255.0f;
        l.r = e.r * k;
        l.g = e.g * k;
        l.b = e.b * k;
        l.range = std::max(1, e.lightRange) * kUnit;
        l.cone = (float)e.lightAngle;
        g_lights.push_back(l);
        if (type == 0 && !sun) { sun = true; g_light = l.dir * -1.0f; }
    }
}

/* A scene with no lights gets the same sun and lamp as the 3D starter.
   One hard sun leaves every face a single shade. The lamp falls off across
   a face, which is the smooth look. */
static void add_default_lights() {
    if (!g_lights.empty()) return;
    Light sun{};
    sun.type = 0;
    sun.dir = kDefaultLight * -1.0f;
    float k = 60.0f / 100.0f / 255.0f;
    sun.r = 255.0f * k;
    sun.g = 244.0f * k;
    sun.b = 214.0f * k;
    g_lights.push_back(sun);
    g_light = kDefaultLight;

    V3 gap = C.eye - C.target;
    float dist = std::sqrt(dot(gap, gap));
    if (dist < 4.0f) dist = 4.0f;
    Light lamp{};
    lamp.type = 1;
    lamp.pos = C.target + v3(dist * 0.15f, dist * 0.45f, dist * 0.10f);
    float ki = 160.0f / 100.0f / 255.0f;
    lamp.r = 255.0f * ki;
    lamp.g = 170.0f * ki;
    lamp.b = 90.0f * ki;
    lamp.range = dist * 1.1f;
    lamp.dir = v3(0, -1, 0);
    g_lights.push_back(lamp);
}

/* ----- OpenGL backend -------------------------------------------------------------
 * The same picture drawn by the GPU: an off-screen multisampled framebuffer on a
 * hidden window's context, resolved and read back into the DIB that is blitted
 * to the back buffer. The rasterizer above stays as the fallback for machines
 * without framebuffer objects. */

static int g_viewSlot = -1;

namespace glb {

constexpr GLenum kFramebuffer = 0x8D40, kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9;
constexpr GLenum kRenderbuffer = 0x8D41, kColorAttachment0 = 0x8CE0, kDepthAttachment = 0x8D00;
constexpr GLenum kStencilAttachment = 0x8D20, kDepth24Stencil8 = 0x88F0, kComplete = 0x8CD5;
constexpr GLenum kMaxSamples = 0x8D57, kBgra = 0x80E1, kRgba8 = 0x8058, kMultisample = 0x809D;

typedef void   (APIENTRY *GenFn)(GLsizei, GLuint*);
typedef void   (APIENTRY *DelFn)(GLsizei, const GLuint*);
typedef void   (APIENTRY *BindFn)(GLenum, GLuint);
typedef void   (APIENTRY *StorageMsFn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *AttachFn)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *StatusFn)(GLenum);
typedef void   (APIENTRY *BlitFn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

struct State {
    bool  tried = false, ok = false;
    HWND  wnd = nullptr;
    HDC   dc = nullptr;
    HGLRC rc = nullptr;
    GenFn genFb = nullptr, genRb = nullptr;
    DelFn delFb = nullptr, delRb = nullptr;
    BindFn bindFb = nullptr, bindRb = nullptr;
    StorageMsFn storageMs = nullptr;
    AttachFn attach = nullptr;
    StatusFn status = nullptr;
    BlitFn blit = nullptr;
    GLuint fbo = 0, rbColor = 0, rbDepth = 0;     /* multisampled */
    GLuint fboOut = 0, rbOut = 0;                  /* resolved, read back */
    int    w = 0, h = 0, maxSamples = 0, asked = -1;
    GLuint listBox = 0, listSphere = 0, listCyl = 0;
    std::unordered_map<const Model*, GLuint> modelLists;
    std::vector<int> selected;                     /* drawn this frame and selected */
    bool core = false;                             /* this context is OpenGL 3.3 */
    bool vk = false;                               /* this window is Vulkan 1.4 */
    bool bgra = false;                             /* compatibility context reads DIB order */
};
static State S;

static bool try_core();
static void drop();
static bool is_core();
static bool alive();
static bool begin33(int w, int h, double fov_deg, int br, int bg, int bb);
static void entity33(const Entity& e, int slot);
static void line33(V3 a, V3 b, int r, int g, int bl, int alpha);
static void cover33(const V3* pts, int ntri, int r, int g, int b, int alpha);
static void cover_model33(const Entity& e, int r, int g, int b, int alpha);
static void painted33(const float* v, int ntri);
static void screen_rect33(float x0, float y0, float x1, float y1, int r, int g, int b, int a);
static void sector33(const V3* pts, int ntri, int r, int g, int b, int a);
static void flush_lines33();
static void outline33(int r, int g, int b);

static PROC proc(const char* name) {
    PROC p = wglGetProcAddress(name);
    if (!p) {
        std::string ext = std::string(name) + "EXT";
        p = wglGetProcAddress(ext.c_str());
    }
    return p;
}

static void free_buffers() {
    if (S.fbo)     S.delFb(1, &S.fbo);
    if (S.fboOut)  S.delFb(1, &S.fboOut);
    if (S.rbColor) S.delRb(1, &S.rbColor);
    if (S.rbDepth) S.delRb(1, &S.rbDepth);
    if (S.rbOut)   S.delRb(1, &S.rbOut);
    S.fbo = S.fboOut = S.rbColor = S.rbDepth = S.rbOut = 0;
}

static bool make_buffers(int w, int h, int samples) {
    free_buffers();
    S.genFb(1, &S.fbo);
    S.genFb(1, &S.fboOut);
    S.genRb(1, &S.rbColor);
    S.genRb(1, &S.rbDepth);
    S.genRb(1, &S.rbOut);
    S.bindRb(kRenderbuffer, S.rbColor);
    S.storageMs(kRenderbuffer, samples, kRgba8, w, h);
    S.bindRb(kRenderbuffer, S.rbDepth);
    S.storageMs(kRenderbuffer, samples, kDepth24Stencil8, w, h);
    S.bindRb(kRenderbuffer, S.rbOut);
    S.storageMs(kRenderbuffer, 0, kRgba8, w, h);
    S.bindFb(kFramebuffer, S.fbo);
    S.attach(kFramebuffer, kColorAttachment0, kRenderbuffer, S.rbColor);
    S.attach(kFramebuffer, kDepthAttachment, kRenderbuffer, S.rbDepth);
    S.attach(kFramebuffer, kStencilAttachment, kRenderbuffer, S.rbDepth);
    bool ok = S.status(kFramebuffer) == kComplete;
    S.bindFb(kFramebuffer, S.fboOut);
    S.attach(kFramebuffer, kColorAttachment0, kRenderbuffer, S.rbOut);
    ok = ok && S.status(kFramebuffer) == kComplete;
    return ok;
}

static bool load_fbo() {
    S.genFb = (GenFn)proc("glGenFramebuffers");
    S.genRb = (GenFn)proc("glGenRenderbuffers");
    S.delFb = (DelFn)proc("glDeleteFramebuffers");
    S.delRb = (DelFn)proc("glDeleteRenderbuffers");
    S.bindFb = (BindFn)proc("glBindFramebuffer");
    S.bindRb = (BindFn)proc("glBindRenderbuffer");
    S.storageMs = (StorageMsFn)proc("glRenderbufferStorageMultisample");
    S.attach = (AttachFn)proc("glFramebufferRenderbuffer");
    S.status = (StatusFn)proc("glCheckFramebufferStatus");
    S.blit = (BlitFn)proc("glBlitFramebuffer");
    if (!S.genFb || !S.genRb || !S.delFb || !S.delRb || !S.bindFb || !S.bindRb ||
        !S.storageMs || !S.attach || !S.status || !S.blit) return false;
    GLint ms = 0;
    glGetIntegerv(kMaxSamples, &ms);
    S.maxSamples = ms;
    return true;
}

static bool aa_kind(const Entity& e) {
    if (e.isCamera) return false;
    const std::string& k = e.kind;
    if (k == "empty" || k == "emitter" || k == "sprite2d" || k == "label" || k == "ui_label") return false;
    if (k == "panel" || k == "button" || k == "checkbox" || k == "toggle" ||
        k == "slider" || k == "bar" || k == "field") return false;
    if (k.size() >= 6 && k.compare(0, 6, "light_") == 0) return false;
    if (k.size() >= 3 && k.compare(0, 3, "ui_") == 0) return false;
    if (k == "rect") {
        const std::string& n = e.name;
        if (n == "empty" || n == "panel" || n == "button" || n == "checkbox" ||
            n == "toggle" || n == "slider" || n == "bar" || n == "field") return false;
    }
    return true;
}

/* 8 when any drawn mesh should be smooth, otherwise 0. The project switch is
   the default. An object set to on still asks for samples when the project is off. */
static int aa_samples() {
    int want = g_aa ? 8 : 0;
    if (!want) {
        for (int s = 0; s < (int)g.entities.size(); ++s) {
            const Entity& e = g.entities[s];
            if (e.aa != 1 || !aa_kind(e) || !entity_shown(s)) continue;
            want = 8;
            break;
        }
    }
    if (want > S.maxSamples) want = S.maxSamples;
    if (want < 0) want = 0;
    return want;
}

static void aa_project() {
    if (g_aa) glEnable(kMultisample);
    else glDisable(kMultisample);
}

static void aa_for(const Entity& e) {
    int on = g_aa ? 1 : 0;
    if (e.aa == 0) on = 0;
    else if (e.aa == 1) on = 1;
    if (on) glEnable(kMultisample);
    else glDisable(kMultisample);
}

static bool ensure_buffers(int w, int h, int samples) {
    if (samples < 0) samples = 0;
    if (S.maxSamples > 0 && samples > S.maxSamples) samples = S.maxSamples;
    if (S.fbo && S.w == w && S.h == h && S.asked == samples) return true;
    int tries[3];
    int ntry = 0;
    if (samples > 0) {
        int s8 = std::min(8, S.maxSamples);
        int s4 = std::min(4, S.maxSamples);
        if (samples >= 8 && s8 > 0) tries[ntry++] = s8;
        if (samples >= 4 && s4 > 0 && (ntry == 0 || tries[ntry - 1] != s4)) tries[ntry++] = s4;
    }
    tries[ntry++] = 0;
    for (int i = 0; i < ntry; ++i) {
        if (make_buffers(w, h, tries[i])) {
            S.w = w;
            S.h = h;
            S.asked = samples;
            return true;
        }
    }
    free_buffers();
    S.w = S.h = 0;
    S.asked = -1;
    return false;
}

/* unit shapes, compiled once: half extents / radius 0.5, outward normals */
static void build_lists() {
    S.listBox = glGenLists(3);
    S.listSphere = S.listBox + 1;
    S.listCyl = S.listBox + 2;

    static const float F[6][4][3] = {
        { { .5f,-.5f,-.5f},{ .5f, .5f,-.5f},{ .5f, .5f, .5f},{ .5f,-.5f, .5f} },
        { {-.5f,-.5f, .5f},{-.5f, .5f, .5f},{-.5f, .5f,-.5f},{-.5f,-.5f,-.5f} },
        { {-.5f, .5f,-.5f},{-.5f, .5f, .5f},{ .5f, .5f, .5f},{ .5f, .5f,-.5f} },
        { {-.5f,-.5f, .5f},{-.5f,-.5f,-.5f},{ .5f,-.5f,-.5f},{ .5f,-.5f, .5f} },
        { {-.5f,-.5f, .5f},{ .5f,-.5f, .5f},{ .5f, .5f, .5f},{-.5f, .5f, .5f} },
        { { .5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{-.5f, .5f,-.5f},{ .5f, .5f,-.5f} },
    };
    static const float N[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    /* each face is a grid: lighting is per vertex, and a point or spot light's
       pool has to show on a face much bigger than its reach (the ground) */
    const int GRID = 24;
    glNewList(S.listBox, GL_COMPILE);
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; ++f) {
        glNormal3fv(N[f]);
        auto at = [&](float u, float v) {
            float p[3];
            for (int c = 0; c < 3; ++c) {
                float a = F[f][0][c] + (F[f][1][c] - F[f][0][c]) * u;
                float b = F[f][3][c] + (F[f][2][c] - F[f][3][c]) * u;
                p[c] = a + (b - a) * v;
            }
            glVertex3fv(p);
        };
        for (int i = 0; i < GRID; ++i) {
            for (int j = 0; j < GRID; ++j) {
                float u0 = (float)i / GRID, u1 = (float)(i + 1) / GRID;
                float v0 = (float)j / GRID, v1 = (float)(j + 1) / GRID;
                at(u0, v0); at(u1, v0); at(u1, v1); at(u0, v1);
            }
        }
    }
    glEnd();
    glEndList();

    const int SEG = 40, RING = 24;
    glNewList(S.listSphere, GL_COMPILE);
    for (int j = 0; j < RING; ++j) {
        glBegin(GL_QUAD_STRIP);
        for (int i = 0; i <= SEG; ++i) {
            for (int k = 0; k < 2; ++k) {
                float th = (float)(j + k) / RING * 3.14159265f;
                float ph = (float)i / SEG * 6.2831853f;
                float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
                glNormal3f(x, y, z);
                glVertex3f(x * .5f, y * .5f, z * .5f);
            }
        }
        glEnd();
    }
    glEndList();

    const int CS = 48;
    glNewList(S.listCyl, GL_COMPILE);
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= CS; ++i) {
        float a = (float)i / CS * 6.2831853f;
        float x = std::cos(a), z = std::sin(a);
        glNormal3f(x, 0, z);
        glVertex3f(x * .5f, .5f, z * .5f);
        glVertex3f(x * .5f, -.5f, z * .5f);
    }
    glEnd();
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap == 0 ? .5f : -.5f;
        glBegin(GL_TRIANGLE_FAN);
        glNormal3f(0, cap == 0 ? 1.0f : -1.0f, 0);
        glVertex3f(0, y, 0);
        for (int i = 0; i <= CS; ++i) {
            float a = (float)i / CS * 6.2831853f;
            glVertex3f(std::cos(a) * .5f, y, std::sin(a) * .5f);
        }
        glEnd();
    }
    glEndList();
}

/* The picture window sits on top of the engine window. Every click and move
   is handed to the parent in the parent's client coordinates, once. */
static int gl_forward_mouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HWND parent = GetParent(hwnd);
    if (!parent) return 0;
    POINT pt{ (LONG)(short)LOWORD(lp), (LONG)(short)HIWORD(lp) };
    ClientToScreen(hwnd, &pt);
    ScreenToClient(parent, &pt);
    SendMessageA(parent, msg, wp, MAKELPARAM(pt.x, pt.y));
    return 1;
}

static LRESULT CALLBACK gl_view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
        msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
        msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ||
        msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL) {
        if (gl_forward_mouse(hwnd, msg, wp, lp)) return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void register_gl_class() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = gl_view_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "EyescireGLView";
    wc.style = CS_OWNDC;
    RegisterClassExA(&wc);
}

static void hide_view() {
    if (S.wnd && IsWindowVisible(S.wnd)) ShowWindow(S.wnd, SW_HIDE);
}

/* The view is a child of the editor, so SwapBuffers shows it. No readback.
   Showing it again every frame makes Windows treat the next click as the
   first click on a new window and drop it. */
static void place(int x, int y, int w, int h) {
    if (!S.wnd) return;
    static int px = -99999, py = -99999, pw = -1, ph = -1;
    if (x == px && y == py && w == pw && h == ph && IsWindowVisible(S.wnd)) return;
    px = x; py = y; pw = w; ph = h;
    SetWindowPos(S.wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static int g_swapApplied = -1;
static int g_swapOk = 0;

static void swap_interval() {
    int want = g_vsync ? 1 : 0;
    if (g_swapApplied == want) return;
    using Fn = BOOL (APIENTRY *)(int);
    auto set = (Fn)wglGetProcAddress("wglSwapIntervalEXT");
    g_swapOk = (set && set(want)) ? 1 : 0;
    g_swapApplied = want;
}

static void swap_interval_reset() {
    g_swapApplied = -1;
    g_swapOk = 0;
}

static bool ready() {
    if (S.tried) return S.ok;
    S.tried = true;
    register_gl_class();
    HWND parent = g.hwnd;
    DWORD style = parent ? (WS_CHILD | WS_CLIPSIBLINGS) : WS_POPUP;
    DWORD ex = parent ? WS_EX_NOACTIVATE : 0;
    S.wnd = CreateWindowExA(ex, "EyescireGLView", "", style, 0, 0, 16, 16, parent, nullptr,
                            GetModuleHandleA(nullptr), nullptr);
    if (!S.wnd) return false;
    if (g_wantVk) {
        if (nexa_vk14::open(S.wnd, g_vsync) && nexa_vk14::scene_ok()) {
            S.vk = true;
            S.ok = true;
            S.maxSamples = 8;
            return true;
        }
        nexa_vk14::shutdown();
        DestroyWindow(S.wnd);
        S.wnd = nullptr;
        g_wantVk = 0;
        g_want33 = 1;
        S.tried = false;
        return ready();
    }
    S.dc = GetDC(S.wnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(S.dc, &pfd);
    if (!pf || !SetPixelFormat(S.dc, pf, &pfd)) return false;
    if (g_want33 && try_core()) {
        S.core = true;
        S.ok = true;
        return true;
    }
    S.rc = wglCreateContext(S.dc);
    if (!S.rc || !wglMakeCurrent(S.dc, S.rc)) return false;
    if (!load_fbo()) return false;
    build_lists();
    S.ok = true;
    return true;
}

static GLuint model_list(const Model& m) {
    auto it = S.modelLists.find(&m);
    if (it != S.modelLists.end()) return it->second;
    GLuint id = glGenLists(1);
    glNewList(id, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (size_t k = 0; k + 2 < m.tri.size(); k += 3) {
        V3 a = m.pos[m.tri[k]], b = m.pos[m.tri[k + 1]], c = m.pos[m.tri[k + 2]];
        V3 n = norm(cross(b - a, c - a));
        glNormal3f(n.x, n.y, n.z);
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);
        glVertex3f(c.x, c.y, c.z);
    }
    glEnd();
    glEndList();
    S.modelLists[&m] = id;
    return id;
}

/* One entity's mesh with its transform; the caller sets color and state. */
static void shape(const Entity& e) {
    float w = (float)e.w, h = (float)e.h, d = (float)depth_of(e);
    if (w <= 0 || h <= 0) return;
    glPushMatrix();
    glTranslatef((e.x + w * 0.5f) * kUnit, -(e.y + h * 0.5f) * kUnit, -(e.z + d * 0.5f) * kUnit);
    float om[16];
    ::orient_matrix(om, e.rotX, e.rotY, e.rotDeg);
    glMultMatrixf(om);
    if (e.kind == "sphere") {
        float dm = std::min(w, h) * kUnit;
        glScalef(dm, dm, dm);
        glCallList(S.listSphere);
    } else if (e.kind == "cylinder") {
        glScalef(w * kUnit, h * kUnit, w * kUnit);
        glCallList(S.listCyl);
    } else if (e.kind == "capsule") {
        glScalef(w * kUnit, h * kUnit, w * kUnit);
        CapsuleLocal cap = capsule_local(w, h);
        glBegin(GL_TRIANGLES);
        capsule_emit(cap, w * kUnit, h * kUnit, w * kUnit, [](float px, float py, float pz, float nx, float ny, float nz) {
            glNormal3f(nx, ny, nz);
            glVertex3f(px, py, pz);
        });
        glEnd();
    } else {
        const Model* m = nullptr;
        if (e.kind == "model" && !e.model.empty()) m = &model_load(model_path(e.model));
        if (m && m->ok) {
            float sz = std::max(w, h) * kUnit;
            glScalef(sz, sz, sz);
            /* models may be open or wound either way: light both sides. The
               projection is mirrored in y (see begin), so front faces are CW. */
            glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
            glCallList(model_list(*m));
            glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
        } else {
            glScalef(w * kUnit, h * kUnit, d * kUnit);
            glCallList(S.listBox);
        }
    }
    glPopMatrix();
}

/* The real mesh, drawn as a translucent coat. A grid-snapped copy fills the
   holes in a hallway or any other concave model. */
static void cover_model(const Entity& e, int r, int g, int b, int alpha) {
    if (S.core) { cover_model33(e, r, g, b, alpha); return; }
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)std::clamp(alpha, 0, 255));
    shape(e);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

static GLuint g_skyTex11[6] = {};
static std::string g_skyStamp11;

static void sky_gl11_reset() {
    for (int i = 0; i < 6; ++i) g_skyTex11[i] = 0;
    g_skyStamp11.clear();
}

/* Six pictures around the camera. Clip z is forced to w so the near plane
   does not cut the box the camera sits inside. */
static void sky_draw11(int br, int bg, int bb) {
    const SkyCpu* cpu = sky_cpu();
    if (!cpu || cpu->face[0].w < 1) {
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glBegin(GL_QUADS);
        glColor3ub((GLubyte)std::min(255, br + 14), (GLubyte)std::min(255, bg + 16), (GLubyte)std::min(255, bb + 22));
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glColor3ub((GLubyte)br, (GLubyte)bg, (GLubyte)bb);
        glVertex2f(1, 1);
        glVertex2f(-1, 1);
        glEnd();
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        return;
    }
    if (g_skyStamp11 != cpu->stamp || !g_skyTex11[0]) {
        if (!g_skyTex11[0]) glGenTextures(6, g_skyTex11);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (int i = 0; i < 6; ++i) {
            const SkyImg& im = cpu->face[i];
            glBindTexture(GL_TEXTURE_2D, g_skyTex11[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, im.w, im.h, 0, GL_RGB, GL_UNSIGNED_BYTE, im.rgb.data());
        }
        g_skyStamp11 = cpu->stamp;
    }
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    double pm[16];
    glGetDoublev(GL_PROJECTION_MATRIX, pm);
    pm[2] = pm[3]; pm[6] = pm[7]; pm[10] = pm[11]; pm[14] = pm[15];
    glLoadMatrixd(pm);
    glMatrixMode(GL_MODELVIEW);
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    static const float q[6][4][5] = {
        { {0,0, 1,-1, 1}, {1,0, 1,-1,-1}, {1,1, 1, 1,-1}, {0,1, 1, 1, 1} },
        { {0,0,-1,-1,-1}, {1,0,-1,-1, 1}, {1,1,-1, 1, 1}, {0,1,-1, 1,-1} },
        { {0,0,-1, 1, 1}, {1,0, 1, 1, 1}, {1,1, 1, 1,-1}, {0,1,-1, 1,-1} },
        { {0,0,-1,-1,-1}, {1,0, 1,-1,-1}, {1,1, 1,-1, 1}, {0,1,-1,-1, 1} },
        { {0,0,-1,-1, 1}, {1,0, 1,-1, 1}, {1,1, 1, 1, 1}, {0,1,-1, 1, 1} },
        { {0,0, 1,-1,-1}, {1,0,-1,-1,-1}, {1,1,-1, 1,-1}, {0,1, 1, 1,-1} },
    };
    for (int f = 0; f < 6; ++f) {
        glBindTexture(GL_TEXTURE_2D, g_skyTex11[f]);
        glBegin(GL_QUADS);
        for (int k = 0; k < 4; ++k) {
            glTexCoord2f(q[f][k][0], q[f][k][1]);
            glVertex3f(q[f][k][2], q[f][k][3], q[f][k][4]);
        }
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    glDepthMask(GL_TRUE);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}


static int vk_frame_begin(int w, int h, double fov_deg, int br, int bg, int bb);
static void vk_frame_entity(const Entity& e, int slot);
static void vk_frame_line(V3 a, V3 b, int r, int g, int bl, int alpha);

static int vk_begin(int w, int h, double fov_deg, int br, int bg, int bb) {
    return vk_frame_begin(w, h, fov_deg, br, bg, bb);
}

static void vk_entity(const Entity& e, int slot) {
    vk_frame_entity(e, slot);
}

static void vk_line(V3 a, V3 b, int r, int g, int bl, int alpha) {
    vk_frame_line(a, b, r, g, bl, alpha);
}

static bool is_vk() { return S.vk; }

static bool begin(int w, int h, double fov_deg, int br, int bg, int bb) {
    place(C.sx, C.sy, w, h);
    /* The window stays the size of the view. High definition draws a small
       view (the editor pane) into a buffer twice as wide and tall, and the
       present scales it back down. Play in 3D is the whole monitor: doubling
       that, on top of multisampling, is a 4K frame of the lit hall. That is
       what stalls the play window while the editor stays smooth. A view that
       is already 1080p or larger is drawn at 1080p and scaled up. */
    int rw = w, rh = h;
    if (g_hd && w < 1920 && h < 1080) {
        rw = w * 2;
        rh = h * 2;
        if (rw > 4096) rw = 4096;
        if (rh > 4096) rh = 4096;
    } else if (w > 1920 || h > 1080) {
        double sx = 1920.0 / (double)w;
        double sy = 1080.0 / (double)h;
        double s = sx < sy ? sx : sy;
        rw = (int)(w * s);
        rh = (int)(h * s);
        if (rw < 1) rw = 1;
        if (rh < 1) rh = 1;
    }
    if (S.core) return begin33(rw, rh, fov_deg, br, bg, bb);
    if (!wglMakeCurrent(S.dc, S.rc) || !ensure_buffers(rw, rh, aa_samples())) return false;
    swap_interval();
    S.selected.clear();
    S.bindFb(kFramebuffer, S.fbo);
    glViewport(0, 0, rw, rh);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    aa_project();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CW);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* glReadPixels returns rows bottom-up and the DIB is top-down, so the whole
       frame is drawn mirrored in y: NDC y = -1 is the top of the picture. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    const double zn = kNear, zf = 4000.0;
    const double top = zn * std::tan(fov_deg * 0.5 * 3.14159265358979 / 180.0);
    const double right = top * (double)w / (double)h;
    glMatrixMode(GL_PROJECTION);
    glScaled(1, -1, 1);
    glFrustum(-right, right, -top, top, zn, zf);
    glMatrixMode(GL_MODELVIEW);
    const float m[16] = {
        C.right.x, C.up.x, -C.fwd.x, 0,
        C.right.y, C.up.y, -C.fwd.y, 0,
        C.right.z, C.up.z, -C.fwd.z, 0,
        0, 0, 0, 1,
    };
    glLoadMatrixf(m);
    sky_draw11(br, bg, bb);
    glTranslatef(-C.eye.x, -C.eye.y, -C.eye.z);

    const float zero[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < 8; ++i) glDisable(GL_LIGHT0 + i);
    /* The stock sun and fill stay until the scene replaces them: a directional
       light replaces the sun, an ambient light the fill. Point and spot lights
       only ever add. */
    float amb[4] = { 0.55f, 0.55f, 0.55f, 1 };
    bool any_ambient = false, any_sun = false;
    for (const Light& l : g_lights) {
        if (l.type == 0) any_sun = true;
        if (l.type == 3) {
            if (!any_ambient) { amb[0] = amb[1] = amb[2] = 0; any_ambient = true; }
            amb[0] += l.r; amb[1] += l.g; amb[2] += l.b;
        }
    }
    /* Same fill as the 3.3 path: a brighter ambient gives the sun less of the
       picture, so the intensity change is visible on faces the sun already lit. */
    float directScale = 1.0f;
    if (any_ambient) {
        float lift = (amb[0] + amb[1] + amb[2]) / 3.0f;
        if (lift < 0) lift = 0;
        if (lift > 1) lift = 1;
        directScale = 1.0f - lift;
    }
    int n = 0;
    auto plain = [&](GLenum id) {
        glLightfv(id, GL_AMBIENT, zero);
        glLightfv(id, GL_SPECULAR, zero);
        glLightf(id, GL_SPOT_CUTOFF, 180.0f);
        glLightf(id, GL_CONSTANT_ATTENUATION, 1.0f);
        glLightf(id, GL_LINEAR_ATTENUATION, 0.0f);
        glLightf(id, GL_QUADRATIC_ATTENUATION, 0.0f);
    };
    bool has_lamp = false;
    for (const Light& l : g_lights) if (l.type == 1 || l.type == 2) has_lamp = true;
    if (!any_sun && !has_lamp) {
        const GLenum id = GL_LIGHT0 + n++;
        const float p[4] = { kDefaultLight.x, kDefaultLight.y, kDefaultLight.z, 0 };
        const float dif[4] = { 0.95f * directScale, 0.95f * directScale, 0.95f * directScale, 1 };
        plain(id);
        glLightfv(id, GL_POSITION, p);
        glLightfv(id, GL_DIFFUSE, dif);
        glEnable(id);
    }
    for (const Light& l : g_lights) {
        if (l.type == 3) continue;
        if (n >= 8) continue;      /* fixed-function limit */
        const GLenum id = GL_LIGHT0 + n++;
        const float dif[4] = { l.r * directScale, l.g * directScale, l.b * directScale, 1 };
        plain(id);
        glLightfv(id, GL_DIFFUSE, dif);
        if (l.type == 0) {
            const float p[4] = { -l.dir.x, -l.dir.y, -l.dir.z, 0 };
            glLightfv(id, GL_POSITION, p);
        } else {
            const float p[4] = { l.pos.x, l.pos.y, l.pos.z, 1 };
            glLightfv(id, GL_POSITION, p);
            /* 1 / (1 + (d / range)^2): half the light left at the range */
            glLightf(id, GL_QUADRATIC_ATTENUATION, 1.0f / (l.range * l.range));
            if (l.type == 2) {
                const float d[3] = { l.dir.x, l.dir.y, l.dir.z };
                glLightfv(id, GL_SPOT_DIRECTION, d);
                glLightf(id, GL_SPOT_CUTOFF, std::clamp(l.cone * 0.5f, 1.0f, 90.0f));
                glLightf(id, GL_SPOT_EXPONENT, 6.0f);
            }
        }
        glEnable(id);
    }
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
    glEnable(GL_LIGHTING);
    glEnable(GL_NORMALIZE);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    return true;
}

static void entity(const Entity& e, int slot) {
    aa_for(e);
    if (S.core) { entity33(e, slot); aa_project(); return; }
    int aa = std::clamp(e.a, 0, 255);
    if (aa < 255) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4ub((GLubyte)e.r, (GLubyte)e.g, (GLubyte)e.b, (GLubyte)aa);
    } else {
        glDisable(GL_BLEND);
        glColor3ub((GLubyte)e.r, (GLubyte)e.g, (GLubyte)e.b);
    }
    shape(e);
    if (aa < 255) glDisable(GL_BLEND);
    if (e.selected) S.selected.push_back(slot);
    aa_project();
}

/* Translucent coat in world space. pts is 3 vertices per triangle. */
static void cover(const V3* pts, int ntri, int r, int g_, int b, int alpha) {
    if (!pts || ntri <= 0) return;
    if (S.core) { cover33(pts, ntri, r, g_, b, alpha); return; }
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glColor4ub((GLubyte)r, (GLubyte)g_, (GLubyte)b, (GLubyte)std::clamp(alpha, 0, 255));
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < ntri * 3; ++i) glVertex3f(pts[i].x, pts[i].y, pts[i].z);
    glEnd();
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

/* Flat-shaded triangles, always in front. v is 6 floats per vertex: x y z, color 0..1. */
static void painted(const float* v, int ntri) {
    if (!v || ntri <= 0) return;
    if (S.core) { painted33(v, ntri); return; }
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < ntri * 3; ++i) {
        const float* p = v + i * 6;
        glColor3ub((GLubyte)(p[3] * 255), (GLubyte)(p[4] * 255), (GLubyte)(p[5] * 255));
        glVertex3f(p[0], p[1], p[2]);
    }
    glEnd();
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

/* Translucent triangle fan, drawn in front of the scene. pts is 3 verts per triangle. */
static void sector(const V3* pts, int ntri, int r, int g_, int b, int a) {
    if (!pts || ntri <= 0) return;
    if (S.core) { sector33(pts, ntri, r, g_, b, a); return; }
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4ub((GLubyte)r, (GLubyte)g_, (GLubyte)b, (GLubyte)std::clamp(a, 0, 255));
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < ntri * 3; ++i) glVertex3f(pts[i].x, pts[i].y, pts[i].z);
    glEnd();
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glDisable(GL_BLEND);
}

/* Translucent rectangle in view pixels (origin top-left of the 3D picture).
   NDC y = -1 is the top, matching the flipped present. */
static void screen_rect(float x0, float y0, float x1, float y1, int r, int g_, int b, int a) {
    if (S.core) { screen_rect33(x0, y0, x1, y1, r, g_, b, a); return; }
    a = std::clamp(a, 0, 255);
    if (a <= 0) return;
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glColor4ub((GLubyte)r, (GLubyte)g_, (GLubyte)b, (GLubyte)a);
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

static void line(V3 a, V3 b, int r, int g_, int bl, int alpha) {
    if (S.core) { line33(a, b, r, g_, bl, alpha); return; }
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glLineWidth(1.0f);
    glColor4ub((GLubyte)r, (GLubyte)g_, (GLubyte)bl, (GLubyte)std::clamp(alpha, 0, 255));
    glBegin(GL_LINES);
    glVertex3f(a.x, a.y, a.z);
    glVertex3f(b.x, b.y, b.z);
    glEnd();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

/* Selection outline: stencil the selected meshes, then draw them again as thick
   wireframe where the stencil is clear, which leaves only the silhouette. */
static void outline(int r, int g_, int b) {
    if (S.core) { outline33(r, g_, b); return; }
    if (S.selected.empty()) return;
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    for (int s : S.selected) if (active_slot(s)) shape(g.entities[s]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(4.0f);
    glColor3ub((GLubyte)r, (GLubyte)g_, (GLubyte)b);
    for (int s : S.selected) if (active_slot(s)) shape(g.entities[s]);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.0f);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
}

static std::unordered_map<int, GLuint> g_partTex;

static void part_gl_forget() {
    for (auto& kv : g_partTex) if (kv.second) glDeleteTextures(1, &kv.second);
    g_partTex.clear();
}

static GLuint part_gl_tex(int imageId) {
    auto it = g_partTex.find(imageId);
    if (it != g_partTex.end()) return it->second;
    const PartPix* pix = part_pixels(imageId);
    if (!pix || pix->w < 1 || pix->h < 1) return 0;
    std::vector<unsigned char> flip((size_t)pix->w * pix->h * 4);
    for (int y = 0; y < pix->h; ++y)
        std::memcpy(flip.data() + (size_t)(pix->h - 1 - y) * pix->w * 4,
                    pix->rgba.data() + (size_t)y * pix->w * 4, (size_t)pix->w * 4);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLint wrap = S.core ? 0x812F : 0x2900;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pix->w, pix->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, flip.data());
    g_partTex[imageId] = tex;
    return tex;
}

static void particles33(const PartDraw* draws, int n);

/* Camera-facing copies of each particle's picture, tinted and faded. */
static void particles_gl() {
    PartDraw buf[kPartMax];
    int n = part_collect_3d(buf, kPartMax);
    if (n <= 0) return;
    if (S.core) { particles33(buf, n); return; }
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    for (int i = 0; i < n; ++i) {
        const PartDraw& d = buf[i];
        GLuint tex = part_gl_tex(d.image);
        if (!tex) continue;
        glBindTexture(GL_TEXTURE_2D, tex);
        glColor4ub((GLubyte)d.r, (GLubyte)d.g, (GLubyte)d.b, (GLubyte)d.a);
        float cx = d.x * kUnit, cy = -d.y * kUnit, cz = -d.z * kUnit, hs = d.size * kUnit * 0.5f;
        V3 rgt = C.right * hs, up = C.up * hs;
        V3 p0 = v3(cx, cy, cz) - rgt - up;
        V3 p1 = v3(cx, cy, cz) + rgt - up;
        V3 p2 = v3(cx, cy, cz) + rgt + up;
        V3 p3 = v3(cx, cy, cz) - rgt + up;
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(p0.x, p0.y, p0.z);
        glTexCoord2f(1, 0); glVertex3f(p1.x, p1.y, p1.z);
        glTexCoord2f(1, 1); glVertex3f(p2.x, p2.y, p2.z);
        glTexCoord2f(0, 1); glVertex3f(p3.x, p3.y, p3.z);
        glEnd();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
    glColor3f(1, 1, 1);
}

/* Queue the resolve and return. The window is presented later, after the
   editor has drawn the gizmos, so that work overlaps this GPU job. */
static void finish() {
    if (S.core) flush_lines33();
    swap_interval();
    S.bindFb(kReadFramebuffer, S.fbo);
    S.bindFb(kDrawFramebuffer, S.fboOut);
    S.blit(0, 0, S.w, S.h, 0, 0, S.w, S.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    S.bindFb(kFramebuffer, 0);
    glFlush();
}

static GLuint ovTex = 0;
static int ovW = 0, ovH = 0;

/* Flip the resolved frame onto the view window and draw the editor's
   viewport-space HUD on top. bits is a top-down BGRA image with alpha,
   or null when the HUD is empty. */
static void show_window(const void* bits, int w, int h) {
    if (S.vk) {
        nexa_vk14::present(bits, w, h);
        return;
    }
    if (!S.wnd || !wglMakeCurrent(S.dc, S.rc)) return;
    S.bindFb(kReadFramebuffer, S.fboOut);
    S.bindFb(kDrawFramebuffer, 0);
    glReadBuffer(kColorAttachment0);
    glDrawBuffer(GL_BACK);
    RECT cr{};
    GetClientRect(S.wnd, &cr);
    int ww = cr.right > 0 ? cr.right : S.w;
    int hh = cr.bottom > 0 ? cr.bottom : S.h;
    /* A larger buffer is the high-definition picture. Linear filtering is the
       downsample. A matching size stays nearest so it is not softened. */
    GLenum filt = (S.w == ww && S.h == hh) ? GL_NEAREST : GL_LINEAR;
    S.blit(0, 0, S.w, S.h, 0, hh, ww, 0, GL_COLOR_BUFFER_BIT, filt);
    S.bindFb(kFramebuffer, 0);
    /* The scene buffer can be larger than the window. The menu and gizmos
       are in window pixels, so the draw area has to match the window. */
    glViewport(0, 0, ww, hh);
    if (bits && w > 0 && h > 0 && (!S.core || S.bgra)) {
        using UseFn = void (APIENTRY *)(GLuint);
        auto use = (UseFn)wglGetProcAddress("glUseProgram");
        if (use) use(0);
        if (!ovTex) glGenTextures(1, &ovTex);
        glBindTexture(GL_TEXTURE_2D, ovTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        /* A fresh store each frame. Replacing the live texture waits until
           the previous fullscreen menu upload has finished, which stalls Play. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, kBgra, GL_UNSIGNED_BYTE, bits);
        ovW = w;
        ovH = h;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, ww, hh, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(1, 0); glVertex2f((float)ww, 0);
        glTexCoord2f(1, 1); glVertex2f((float)ww, (float)hh);
        glTexCoord2f(0, 1); glVertex2f(0, (float)hh);
        glEnd();
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    g_glPresented = (g_vsync && g_swapOk) ? 1 : 0;
    SwapBuffers(S.dc);
}

#include "render3d33.inl"

} /* namespace glb */

static bool g_useGL = false;           /* this frame is drawn by glb */
static bool g_useVk = false;           /* this frame is drawn by Vulkan 1.4 */
static std::vector<int> g_drawn;       /* slots drawn this frame, for picking */

static float ray_sphere_t(V3 o, V3 d, V3 c, float R) {
    V3 oc = o - c;
    float A = dot(d, d), B = 2 * dot(oc, d), Cq = dot(oc, oc) - R * R;
    float disc = B * B - 4 * A * Cq;
    if (A < 1e-12f || disc < 0) return -1;
    float s = std::sqrt(disc);
    float t = (-B - s) / (2 * A);
    if (t > 0) return t;
    t = (-B + s) / (2 * A);
    return t > 0 ? t : -1;
}

/* Capsule along Y, segment half-length hy, radius R. t matches the incoming ray. */
static float ray_capsule_t(V3 o, V3 d, float hy, float R) {
    float best = 1e30f;
    float A = d.x * d.x + d.z * d.z;
    if (A > 1e-12f) {
        float B = 2 * (o.x * d.x + o.z * d.z);
        float Cq = o.x * o.x + o.z * o.z - R * R;
        float disc = B * B - 4 * A * Cq;
        if (disc >= 0) {
            float s = std::sqrt(disc);
            float t0 = (-B - s) / (2 * A), t1 = (-B + s) / (2 * A);
            float ts[2] = { t0, t1 };
            for (float t : ts) {
                if (t <= 0) continue;
                float y = o.y + t * d.y;
                if (y >= -hy && y <= hy && t < best) best = t;
            }
        }
    }
    float top = ray_sphere_t(o, d, v3(0, hy, 0), R);
    float bot = ray_sphere_t(o, d, v3(0, -hy, 0), R);
    if (top > 0 && top < best) best = top;
    if (bot > 0 && bot < best) best = bot;
    return best < 1e29f ? best : -1.f;
}

/* Nearest hit of a world-space ray on an entity's shape, or < 0. */
static float ray_entity(const Entity& e, V3 o, V3 dir) {
    float w = (float)e.w, h = (float)e.h, d = (float)depth_of(e);
    if (w <= 0 || h <= 0) return -1;
    V3 c = v3((e.x + w * 0.5f) * kUnit, -(e.y + h * 0.5f) * kUnit, -(e.z + d * 0.5f) * kUnit);
    V3 sc = v3(w * kUnit, h * kUnit, d * kUnit);
    bool sphere = e.kind == "sphere";
    if (sphere) { float dm = std::min(w, h) * kUnit; sc = v3(dm, dm, dm); }
    else if (e.kind == "cylinder" || e.kind == "capsule") sc = v3(w * kUnit, h * kUnit, w * kUnit);
    else if (e.kind == "model") { float sz = std::max(w, h) * kUnit; sc = v3(sz, sz, sz); }
    float om[16];
    ::orient_matrix(om, e.rotX, e.rotY, e.rotDeg);
    /* into the shape's unit space: undo translate, rotate, scale */
    auto local = [&](V3 p) {
        return v3((p.x * om[0] + p.y * om[1] + p.z * om[2]) / sc.x,
                   (p.x * om[4] + p.y * om[5] + p.z * om[6]) / sc.y,
                   (p.x * om[8] + p.y * om[9] + p.z * om[10]) / sc.z);
    };
    V3 lo = local(o - c), ld = local(dir);
    if (sphere) {
        float A = dot(ld, ld), B = 2 * dot(lo, ld), Cq = dot(lo, lo) - 0.25f;
        float disc = B * B - 4 * A * Cq;
        if (A < 1e-12f || disc < 0) return -1;
        float t = (-B - std::sqrt(disc)) / (2 * A);
        return t > 0 ? t : -1;
    }
    if (e.kind == "capsule") {
        V3 rs = v3(lo.x * sc.x, lo.y * sc.y, lo.z * sc.z);
        V3 rd = v3(ld.x * sc.x, ld.y * sc.y, ld.z * sc.z);
        CapsuleLocal cg = capsule_local(w, h);
        return ray_capsule_t(rs, rd, cg.cy * sc.y, cg.rx * sc.x);
    }
    float t0 = 0, t1 = 1e30f;
    const float po[3] = { lo.x, lo.y, lo.z }, pd[3] = { ld.x, ld.y, ld.z };
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(pd[k]) < 1e-9f) {
            if (po[k] < -0.5f || po[k] > 0.5f) return -1;
            continue;
        }
        float ta = (-0.5f - po[k]) / pd[k], tb = (0.5f - po[k]) / pd[k];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return -1;
    }
    return t0;
}

/* Flat pictures in the software picture, one quad per particle. */
static void stamp_particles() {
    PartDraw buf[kPartMax];
    int n = part_collect_3d(buf, kPartMax);
    for (int i = 0; i < n; ++i) {
        const PartDraw& d = buf[i];
        const PartPix* pix = part_pixels(d.image);
        if (!pix) continue;
        float cx = d.x * kUnit, cy = -d.y * kUnit, cz = -d.z * kUnit;
        if (!engine_r3d_project(cx, cy, cz)) continue;
        float px = (float)g_px, py = (float)g_py;
        if (!engine_r3d_project(cx + d.size * kUnit, cy, cz)) continue;
        int sw = (int)std::fabs((float)g_px - px);
        if (sw < 2) sw = 2;
        if (sw > 512) sw = 512;
        int x0 = (int)std::floor(px) - sw / 2 - C.sx;
        int y0 = (int)std::floor(py) - sw / 2 - C.sy;
        for (int y = 0; y < sw; ++y) {
            int ty = y0 + y;
            if (ty < 0 || ty >= T.h) continue;
            int sy = y * pix->h / sw;
            if (sy >= pix->h) sy = pix->h - 1;
            for (int x = 0; x < sw; ++x) {
                int tx = x0 + x;
                if (tx < 0 || tx >= T.w) continue;
                int sx = x * pix->w / sw;
                if (sx >= pix->w) sx = pix->w - 1;
                const unsigned char* s = pix->rgba.data() + ((size_t)sy * pix->w + sx) * 4;
                int sa = s[3] * d.a / 255;
                if (sa < 1) continue;
                int sr = s[0] * d.r / 255, sg = s[1] * d.g / 255, sb = s[2] * d.b / 255;
                size_t k = (size_t)ty * T.w + tx;
                uint32_t old = T.px[k];
                int orr = (int)((old >> 16) & 255), og = (int)((old >> 8) & 255), ob = (int)(old & 255);
                int inv = 255 - sa;
                T.px[k] = pack((sr * sa + orr * inv) / 255, (sg * sa + og * inv) / 255, (sb * sa + ob * inv) / 255);
            }
        }
    }
}

/* direction of the ray through back-buffer pixel (x, y) */
static V3 pixel_ray(int x, int y) {
    float cx = (x - C.sx + 0.5f - T.w * 0.5f) / C.f;
    float cy = -(y - C.sy + 0.5f - T.h * 0.5f) / C.f;
    return norm(C.right * cx + C.up * cy + C.fwd);
}

} /* namespace r3d */

static int g_hidePawn = 0;
static int g_lookCam = -1; /* -1 unknown this frame, 0 none, 1 a look camera */

extern "C" void engine_r3d_begin(int x, int y, int w, int h,
                                 double ex, double ey, double ez,
                                 double tx, double ty, double tz,
                                 double fov_deg, int br, int bg, int bb) {
    using namespace r3d;
    C.valid = false;
    if (w < 8 || h < 8) return;
    ensure_target(w, h);
    if (!T.px) return;
    C.eye = v3((float)ex, (float)ey, (float)ez);
    C.target = v3((float)tx, (float)ty, (float)tz);
    C.fwd = norm(v3((float)(tx - ex), (float)(ty - ey), (float)(tz - ez)));
    /* World up. A hard swap when the view is steep rolls the picture; the
       horizontal right stays valid until the view is exactly straight up. */
    V3 right = cross(C.fwd, v3(0, 1, 0));
    if (dot(right, right) < 1e-8f) right = cross(C.fwd, v3(0, 0, 1));
    C.right = norm(right);
    C.up = cross(C.right, C.fwd);
    C.f = (h * 0.5f) / (float)std::tan(fov_deg * 0.5 * 3.14159265358979 / 180.0);
    C.sx = x;
    C.sy = y;
    C.valid = true;
    g_drawn.clear();
    g_lookCam = -1;
    collect_lights();
    add_default_lights();
    g_useVk = false;
    g_useGL = false;
    if (glb::ready() && glb::is_vk()) {
        g_useVk = glb::vk_begin(w, h, fov_deg, br, bg, bb) != 0;
        if (!g_useVk) C.valid = false;
        return;
    }
    g_useGL = glb::ready() && glb::begin(w, h, fov_deg, br, bg, bb);
    if (g_useGL) return;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            float cx = (xx + 0.5f - w * 0.5f) / C.f;
            float cy = -(yy + 0.5f - h * 0.5f) / C.f;
            V3 d = norm(C.right * cx + C.up * cy + C.fwd);
            int sr, sg, sb;
            sky_sample(d.x, d.y, d.z, sr, sg, sb);
            T.px[(size_t)yy * w + xx] = pack(sr, sg, sb);
        }
    }
    std::fill(T.invz.begin(), T.invz.end(), 0.0f);
    std::fill(T.ids.begin(), T.ids.end(), -1);
}

static bool pawn_hidden(const Entity& e) {
    if (!g_hidePawn) return false;
    if (g_lookCam < 0) {
        g_lookCam = 0;
        for (const Entity& c : g.entities) {
            if (c.active && c.isCamera && c.text == "look") { g_lookCam = 1; break; }
        }
    }
    if (!g_lookCam) return false;
    if (e.name == "hero") return true;
    return e.script.find("player.nxa") != std::string::npos;
}

extern "C" void engine_r3d_hide_pawn(int on) { g_hidePawn = on ? 1 : 0; }
extern "C" void engine_r3d_view_hold(int slot) { r3d::g_viewSlot = slot; }

extern "C" void engine_r3d_entity(int s) {
    using namespace r3d;
    if (!C.valid || !active_slot(s) || !entity_shown(s)) return;
    const Entity& e = g.entities[s];
    if (pawn_hidden(e)) return;
    meshA = std::clamp(e.a, 0, 255);
    g_drawn.push_back(s);
    if (g_useVk) { glb::vk_entity(e, s); return; }
    if (g_useGL) { glb::entity(e, s); return; }
    float w = (float)e.w, h = (float)e.h, d = (float)depth_of(e);
    if (w <= 0 || h <= 0) return;
    Xform xf;
    xf.center = v3((e.x + w * 0.5f) * kUnit, -(e.y + h * 0.5f) * kUnit, -(e.z + d * 0.5f) * kUnit);
    xform_orient(xf, e);
    if (e.kind == "sphere") {
        float dm = std::min(w, h) * kUnit;
        xf.scale = v3(dm, dm, dm);
        mesh_sphere(xf, e.r, e.g, e.b, s);
    } else if (e.kind == "cylinder") {
        xf.scale = v3(w * kUnit, h * kUnit, w * kUnit);
        mesh_cylinder(xf, e.r, e.g, e.b, s);
    } else if (e.kind == "capsule") {
        xf.scale = v3(w * kUnit, h * kUnit, w * kUnit);
        mesh_capsule(xf, w, h, e.r, e.g, e.b, s);
    } else if (e.kind == "model") {
        const Model* m = nullptr;
        if (!e.model.empty()) m = &model_load(model_path(e.model));
        if (m && m->ok) {
            float sz = std::max(w, h) * kUnit;
            xf.scale = v3(sz, sz, sz);
            mesh_model(*m, xf, e.r, e.g, e.b, s);
        } else {
            xf.scale = v3(w * kUnit, h * kUnit, d * kUnit);
            mesh_box(xf, e.r, e.g, e.b, s);
        }
    } else {
        xf.scale = v3(w * kUnit, h * kUnit, d * kUnit);
        mesh_box(xf, e.r, e.g, e.b, s);
    }
}

/* Solid coat on the real mesh, so a hallway stays hollow. */
extern "C" void engine_r3d_hitbox(int s) {
    using namespace r3d;
    if (!C.valid || !active_slot(s)) return;
    const Entity& e = g.entities[s];
    if (e.hitbox <= 0 || e.kind != "model" || e.model.empty()) return;
    float w = (float)e.w, h = (float)e.h;
    if (w <= 0 || h <= 0) return;
    if (g_useGL || g_useVk) {
        if (g_useVk) glb::vk_frame_cover_model(e, 70, 210, 120, 90);
        else {
            glb::aa_for(e);
            glb::cover_model(e, 70, 210, 120, 90);
            glb::aa_project();
        }
        return;
    }
    const Model* m = &model_load(model_path(e.model));
    if (!m->ok) return;
    float d = (float)depth_of(e);
    Xform xf;
    xf.center = v3((e.x + w * 0.5f) * kUnit, -(e.y + h * 0.5f) * kUnit, -(e.z + d * 0.5f) * kUnit);
    xform_orient(xf, e);
    float sz = std::max(w, h) * kUnit;
    xf.scale = v3(sz, sz, sz);
    int saved = meshA;
    meshA = 90;
    mesh_model(*m, xf, 70, 210, 120, -1);
    meshA = saved;
}

/* Cone from base center to tip, flat-shaded so it reads as a solid arrow head.
   Drawn in front of the scene; it does not steal entity picking. */
extern "C" void engine_r3d_cone(double bx, double by, double bz,
                                double tx, double ty, double tz,
                                double radius, int r, int g, int b) {
    using namespace r3d;
    if (!C.valid || radius <= 0) return;
    V3 base = v3((float)bx, (float)by, (float)bz);
    V3 tip = v3((float)tx, (float)ty, (float)tz);
    V3 axis = tip - base;
    float len2 = dot(axis, axis);
    if (len2 < 1e-10f) return;
    float len = std::sqrt(len2);
    V3 dir = axis * (1.0f / len);
    V3 helper = std::fabs(dir.y) < 0.85f ? v3(0, 1, 0) : v3(1, 0, 0);
    V3 side = norm(cross(dir, helper));
    V3 up = cross(side, dir);
    V3 light = norm(v3(0.35f, 0.82f, 0.45f));
    float rad = (float)radius;
    const int SEG = 12;
    std::vector<float> verts;
    verts.reserve((size_t)SEG * 2 * 18);
    auto push = [&](V3 p, float shade) {
        float cr = std::clamp(r * shade, 0.0f, 255.0f) / 255.0f;
        float cg = std::clamp(g * shade, 0.0f, 255.0f) / 255.0f;
        float cb = std::clamp(b * shade, 0.0f, 255.0f) / 255.0f;
        float s[6] = { p.x, p.y, p.z, cr, cg, cb };
        verts.insert(verts.end(), s, s + 6);
    };
    auto face = [&](V3 a, V3 b2, V3 c, V3 n) {
        float d = dot(n, light);
        if (d < 0) d = 0;
        float shade = 0.42f + 0.58f * d;
        push(a, shade); push(b2, shade); push(c, shade);
    };
    for (int i = 0; i < SEG; ++i) {
        float a0 = (float)i / SEG * 6.2831853f;
        float a1 = (float)(i + 1) / SEG * 6.2831853f;
        V3 c0 = side * std::cos(a0) + up * std::sin(a0);
        V3 c1 = side * std::cos(a1) + up * std::sin(a1);
        V3 p0 = base + c0 * rad;
        V3 p1 = base + c1 * rad;
        V3 n = norm(cross(p0 - tip, p1 - tip));
        if (dot(n, c0) < 0) n = n * -1.0f;
        face(tip, p0, p1, n);
        face(base, p1, p0, dir * -1.0f);
    }
    int ntri = (int)verts.size() / 18;
    if (ntri <= 0) return;
    if (g_useVk) { glb::vk_frame_painted(verts.data(), ntri); return; }
    if (g_useGL) {
        glb::painted(verts.data(), ntri);
        return;
    }
    int saved = g_paintOver;
    g_paintOver = 1;
    int savedA = meshA;
    meshA = 255;
    for (int t = 0; t < ntri; ++t) {
        const float* p = verts.data() + (size_t)t * 18;
        auto at = [&](int k) { return v3(p[k * 6], p[k * 6 + 1], p[k * 6 + 2]); };
        int cr = (int)(p[3] * 255), cg = (int)(p[4] * 255), cb = (int)(p[5] * 255);
        V3 n = g_light;
        draw_tri_world(at(0), at(1), at(2), n, n, n, cr, cg, cb, -1);
    }
    meshA = savedA;
    g_paintOver = saved;
}

/* Depth-tested line in world units; a > 0 blends (0..255) over what's there. */
/* Filled angle of a rotate ring. ang0 and sweep are degrees around that axis. */
extern "C" void engine_r3d_sector(double cx, double cy, double cz, int axis,
                                  double ang0, double sweep, double radius,
                                  int r, int g, int b, int a) {
    using namespace r3d;
    if (!C.valid || radius <= 0 || a <= 0) return;
    if (axis < 1 || axis > 3) return;
    if (std::fabs(sweep) < 0.25) return;
    int steps = (int)std::ceil(std::fabs(sweep) / 6.0);
    if (steps < 1) steps = 1;
    if (steps > 60) steps = 60;
    constexpr float k = 0.017453292519943295f;
    float a0 = (float)ang0 * k;
    float step = (float)sweep * k / (float)steps;
    float rad = (float)radius;
    V3 center = v3((float)cx, (float)cy, (float)cz);
    auto at = [&](float ang) {
        float cs = std::cos(ang), sn = std::sin(ang);
        if (axis == 1) return v3(center.x, center.y + rad * cs, center.z + rad * sn);
        if (axis == 2) return v3(center.x + rad * sn, center.y, center.z + rad * cs);
        return v3(center.x + rad * cs, center.y + rad * sn, center.z);
    };
    std::vector<V3> pts;
    pts.reserve((size_t)steps * 3);
    for (int i = 0; i < steps; ++i) {
        pts.push_back(center);
        pts.push_back(at(a0 + step * i));
        pts.push_back(at(a0 + step * (i + 1)));
    }
    int ntri = steps;
    if (g_useVk) { glb::vk_frame_sector(pts.data(), ntri, r, g, b, a); return; }
    if (g_useGL) {
        glb::sector(pts.data(), ntri, r, g, b, a);
        return;
    }
    int saved = g_paintOver;
    g_paintOver = 1;
    int savedA = meshA;
    meshA = std::clamp(a, 0, 255);
    for (int t = 0; t < ntri; ++t) {
        V3 p0 = pts[(size_t)t * 3], p1 = pts[(size_t)t * 3 + 1], p2 = pts[(size_t)t * 3 + 2];
        draw_tri_cam(CV{ to_cam(p0), 1.f }, CV{ to_cam(p1), 1.f }, CV{ to_cam(p2), 1.f }, r, g, b, -1);
    }
    meshA = savedA;
    g_paintOver = saved;
}

/* Screen-space tint in back-buffer pixels, blended into the 3D picture. */
extern "C" void engine_r3d_screen_rect(int x, int y, int w, int h, int r, int g_, int b, int a) {
    using namespace r3d;
    if (!C.valid || w <= 0 || h <= 0) return;
    a = std::clamp(a, 0, 255);
    if (a <= 0) return;
    int x0 = x - C.sx;
    int y0 = y - C.sy;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > T.w) x1 = T.w;
    if (y1 > T.h) y1 = T.h;
    if (x1 <= x0 || y1 <= y0) return;
    if (g_useGL || g_useVk) {
        float nx0 = (float)x0 / (float)T.w * 2.0f - 1.0f;
        float nx1 = (float)x1 / (float)T.w * 2.0f - 1.0f;
        float ny0 = (float)y0 / (float)T.h * 2.0f - 1.0f;
        float ny1 = (float)y1 / (float)T.h * 2.0f - 1.0f;
        if (g_useVk) glb::vk_frame_screen(nx0, ny0, nx1, ny1, r, g_, b, a);
        else glb::screen_rect(nx0, ny0, nx1, ny1, r, g_, b, a);
        return;
    }
    int ia = 255 - a;
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t* row = T.px + (size_t)yy * T.w;
        for (int xx = x0; xx < x1; ++xx) {
            uint32_t dst = row[xx];
            int dr = (int)((dst >> 16) & 255);
            int dg = (int)((dst >> 8) & 255);
            int db = (int)(dst & 255);
            int nr = (dr * ia + r * a) / 255;
            int ng = (dg * ia + g_ * a) / 255;
            int nb = (db * ia + b * a) / 255;
            row[xx] = pack(nr, ng, nb);
        }
    }
}

/* Depth-tested line in world units; a > 0 blends (0..255) over what's there. */
extern "C" void engine_r3d_line(double x1, double y1, double z1, double x2, double y2, double z2,
                                int r, int g_, int b, int a) {
    using namespace r3d;
    if (!C.valid) return;
    if (g_useVk) {
        glb::vk_line(v3((float)x1, (float)y1, (float)z1), v3((float)x2, (float)y2, (float)z2), r, g_, b, a);
        return;
    }
    if (g_useGL) {
        glb::line(v3((float)x1, (float)y1, (float)z1), v3((float)x2, (float)y2, (float)z2), r, g_, b, a);
        return;
    }
    V3 p = to_cam(v3((float)x1, (float)y1, (float)z1));
    V3 q = to_cam(v3((float)x2, (float)y2, (float)z2));
    if (p.z < kNear && q.z < kNear) return;
    if (p.z < kNear) p = p + (q - p) * ((kNear - p.z) / (q.z - p.z));
    if (q.z < kNear) q = q + (p - q) * ((kNear - q.z) / (p.z - q.z));
    SV s = to_screen(p, 1), e = to_screen(q, 1);
    float dx = e.x - s.x, dy = e.y - s.y;
    int steps = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)));
    if (steps > 8000) steps = 8000;
    if (steps < 1) steps = 1;
    int al = std::clamp(a, 0, 255);
    for (int i = 0; i <= steps; ++i) {
        float t = (float)i / steps;
        int x = (int)(s.x + dx * t), y = (int)(s.y + dy * t);
        if (x < 0 || y < 0 || x >= T.w || y >= T.h) continue;
        size_t k = (size_t)y * T.w + x;
        float iz = s.iz + (e.iz - s.iz) * t;
        if (iz * 1.002f < T.invz[k]) continue;
        uint32_t o = T.px[k];
        int orr = (o >> 16) & 255, og = (o >> 8) & 255, ob = o & 255;
        T.px[k] = pack(orr + (r - orr) * al / 255, og + (g_ - og) * al / 255, ob + (b - ob) * al / 255);
    }
}

static bool g_glFrame = false;
static HWND g_ov = nullptr;

/* Outline every selected entity's silhouette (outline_rgb >= 0), then show. */
extern "C" void engine_r3d_end(int or_, int og, int ob) {
    using namespace r3d;
    if (!C.valid || !g.backDC) { g_viewSlot = -1; return; }
    if (g_useGL || g_useVk) {
        if (g_useGL) {
        glb::particles_gl();
        if (or_ >= 0) glb::outline(or_, og, ob);
        glb::finish();
        } else {
        glb::vk_frame_particles();
        if (or_ >= 0) glb::vk_frame_outline(or_, og, ob);
        }
        /* Key color under the gizmos and HUD. The view window shows the 3D
           picture; present() copies this rectangle on top with the key transparent. */
        round_gfx_drop();
        SelectClipRgn(g.backDC, nullptr);
        HBRUSH br = CreateSolidBrush(RGB(g_frameClearR, g_frameClearG, g_frameClearB));
        RECT rc{ C.sx, C.sy, C.sx + T.w, C.sy + T.h };
        FillRect(g.backDC, &rc, br);
        DeleteObject(br);
        g_glFrame = true;
        g_viewSlot = -1;
        return;
    } else {
        stamp_particles();
        if (or_ >= 0) {
        uint32_t oc = pack(or_, og, ob);
        auto sel = [](int id) { return id >= 0 && active_slot(id) && g.entities[id].selected; };
        for (int y = 0; y < T.h; ++y) {
            for (int x = 0; x < T.w; ++x) {
                size_t k = (size_t)y * T.w + x;
                int id = T.ids[k];
                if (!sel(id)) continue;
                bool edge = x == 0 || y == 0 || x == T.w - 1 || y == T.h - 1 ||
                            T.ids[k - 1] != id || T.ids[k + 1] != id ||
                            T.ids[k - T.w] != id || T.ids[k + T.w] != id;
                if (edge) T.px[k] = oc;
            }
        }
        }
    }
    BitBlt(g.backDC, C.sx, C.sy, T.w, T.h, T.dc, 0, 0, SRCCOPY);
    g_viewSlot = -1;
}

/* Entity under a back-buffer pixel in the last frame, or -1. */
extern "C" int engine_r3d_pick(int x, int y) {
    using namespace r3d;
    if (!C.valid) return -1;
    if (x < C.sx || y < C.sy || x >= C.sx + T.w || y >= C.sy + T.h) return -1;
    V3 dir = pixel_ray(x, y);
    int best = -1;
    float best_t = 1e30f;
    for (int s : g_drawn) {
        if (!active_slot(s)) continue;
        float t = ray_entity(g.entities[s], C.eye, dir);
        if (t >= 0 && t < best_t) { best_t = t; best = s; }
    }
    return best;
}

/* World point -> back-buffer pixel; 0 when behind the camera. */
extern "C" int engine_r3d_project(double wx, double wy, double wz) {
    using namespace r3d;
    if (!C.valid) return 0;
    V3 c = to_cam(v3((float)wx, (float)wy, (float)wz));
    if (c.z < kNear) return 0;
    g_px = C.sx + T.w * 0.5f + c.x * C.f / c.z;
    g_py = C.sy + T.h * 0.5f - c.y * C.f / c.z;
    return 1;
}
extern "C" double engine_r3d_px() { return r3d::g_px; }
extern "C" double engine_r3d_py() { return r3d::g_py; }

/* Ray through back-buffer pixel (x, y) against the plane scene_z = const
   (a plane parallel to the 2D layout). Result in scene px; 0 on a miss. */
extern "C" int engine_r3d_axis_angle(int x, int y, int axis, double wx, double wy, double wz) {
    using namespace r3d;
    if (!C.valid || axis < 1 || axis > 3) return 100000;
    V3 dir = pixel_ray(x, y);
    V3 n = axis == 1 ? v3(1, 0, 0) : axis == 2 ? v3(0, 1, 0) : v3(0, 0, 1);
    V3 c = v3((float)wx, (float)wy, (float)wz);
    float denom = dot(dir, n);
    if (std::fabs(denom) < 1e-5f) return 100000;
    float t = dot(c - C.eye, n) / denom;
    if (t <= 0) return 100000;
    V3 v = (C.eye + dir * t) - c;
    float ang;
    if (axis == 1) ang = std::atan2(v.z, v.y);
    else if (axis == 2) ang = std::atan2(v.x, v.z);
    else ang = std::atan2(v.y, v.x);
    return (int)std::lround(ang * 57.29577951308232);
}

extern "C" int engine_r3d_basis_angle(int x, int y, double cx, double cy, double cz,
                                      double nx, double ny, double nz,
                                      double ux, double uy, double uz,
                                      double vx, double vy, double vz) {
    using namespace r3d;
    if (!C.valid) return 100000;
    V3 n = v3((float)nx, (float)ny, (float)nz);
    float nl = std::sqrt(dot(n, n));
    if (nl < 1e-5f) return 100000;
    n = n * (1.f / nl);
    V3 dir = pixel_ray(x, y);
    float denom = dot(dir, n);
    if (std::fabs(denom) < 1e-5f) return 100000;
    V3 c = v3((float)cx, (float)cy, (float)cz);
    float t = dot(c - C.eye, n) / denom;
    if (t <= 0) return 100000;
    V3 hit = (C.eye + dir * t) - c;
    V3 u = v3((float)ux, (float)uy, (float)uz);
    V3 v = v3((float)vx, (float)vy, (float)vz);
    float ang = std::atan2(dot(hit, v), dot(hit, u));
    return (int)std::lround(ang * 57.29577951308232);
}

extern "C" int engine_r3d_plane_hit(int x, int y, double scene_z) {
    using namespace r3d;
    if (!C.valid) return 0;
    V3 dir = pixel_ray(x, y);
    float pz = -(float)scene_z * kUnit;
    if (std::fabs(dir.z) < 1e-5f) return 0;
    float t = (pz - C.eye.z) / dir.z;
    if (t <= 0) return 0;
    V3 hit = C.eye + dir * t;
    g_hx = hit.x / kUnit;
    g_hy = -hit.y / kUnit;
    return 1;
}
/* The point `dist` world units along the ray through back-buffer pixel (x, y),
   in scene px (read with engine_r3d_hx / hy / hz). */
extern "C" int engine_r3d_ray_point(int x, int y, double dist) {
    using namespace r3d;
    if (!C.valid) return 0;
    V3 p = C.eye + pixel_ray(x, y) * (float)dist;
    g_hx = p.x / kUnit;
    g_hy = -p.y / kUnit;
    g_hz = -p.z / kUnit;
    return 1;
}
extern "C" double engine_r3d_hz() { return r3d::g_hz; }
extern "C" double engine_r3d_hx() { return r3d::g_hx; }
extern "C" double engine_r3d_hy() { return r3d::g_hy; }

/* 0 = not drawn yet, 1 = software rasterizer, 2 = OpenGL, 3 = Vulkan 1.4 */
extern "C" int engine_r3d_backend() {
    if (!r3d::C.valid) return 0;
    if (r3d::g_useVk) return 3;
    return r3d::g_useGL ? 2 : 1;
}

/* "OpenGL 1.1", "OpenGL 3.3", "Vulkan", or "Vulkan 1.4". 1 if the name is one
   of those, 0 otherwise. Takes effect on the next frame. Vulkan 1.4 is what
   opens; if that device cannot be created the view stays on OpenGL 3.3. */
extern "C" int engine_r3d_renderer(const char* name) {
    std::string n;
    if (name) {
        for (const char* p = name; *p; ++p) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            n.push_back(c);
        }
    }
    int want = 0;
    if (n == "opengl" || n == "opengl 1.1") want = 0;
    else if (n == "opengl 3.3") want = 1;
    else if (n == "vulkan" || n == "vulkan 1.4") want = 2;
    else return 0;
    int live = -1;
    if (r3d::glb::alive()) {
        if (r3d::glb::is_vk()) live = 2;
        else if (r3d::glb::is_core()) live = 1;
        else live = 0;
    }
    if (live >= 0 && live != want) r3d::glb::drop();
    r3d::g_want33 = want == 1;
    r3d::g_wantVk = want == 2;
    return 1;
}

static HDC g_ovDC = nullptr;
static HBITMAP g_ovBmp = nullptr;
static HGDIOBJ g_ovOld = nullptr;
static void* g_ovBits = nullptr;
static int g_ovW = 0, g_ovH = 0;

static void ensure_ov_dib(int w, int h) {
    if (g_ovBmp && g_ovW == w && g_ovH == h) return;
    if (g_ovDC && g_ovOld) SelectObject(g_ovDC, g_ovOld);
    if (g_ovBmp) DeleteObject(g_ovBmp);
    if (!g_ovDC) g_ovDC = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_ovBmp = CreateDIBSection(g_ovDC, &bi, DIB_RGB_COLORS, &g_ovBits, nullptr, 0);
    g_ovOld = g_ovBmp ? SelectObject(g_ovDC, g_ovBmp) : nullptr;
    g_ovW = w;
    g_ovH = h;
}

void r3d::overlay_present() {
    if (!g_glFrame) {
        glb::hide_view();
        if (g_ov && IsWindowVisible(g_ov)) ShowWindow(g_ov, SW_HIDE);
        view_hole(0, 0, 0, 0, 0);
        return;
    }
    g_glFrame = false;
    if (!g.hwnd || T.w < 2 || T.h < 2) { view_hole(0, 0, 0, 0, 0); return; }
    view_hole(1, C.sx, C.sy, T.w, T.h);
    ensure_ov_dib(T.w, T.h);
    const void* bits = nullptr;
    if (g_ovBits) {
        BitBlt(g_ovDC, 0, 0, T.w, T.h, g.backDC, C.sx, C.sy, SRCCOPY);
        auto* px = (uint32_t*)g_ovBits;
        uint32_t key = ((uint32_t)g_frameClearR << 16) | ((uint32_t)g_frameClearG << 8) | (uint32_t)g_frameClearB;
        int n = T.w * T.h;
        bool any = false;
        for (int i = 0; i < n; ++i) {
            uint32_t p = px[i] & 0x00FFFFFFu;
            if (p == key) px[i] = 0;
            else { px[i] = p | 0xFF000000u; any = true; }
        }
        if (any) bits = g_ovBits;
    }
    glb::show_window(bits, T.w, T.h);
}

/* What the last frame actually drew with. Empty before the first frame. */
extern "C" const char* engine_r3d_backend_name() {
    if (!r3d::C.valid) return "";
    if (r3d::g_useVk) return "Vulkan 1.4";
    if (r3d::g_useGL && r3d::glb::is_core()) return "OpenGL 3.3";
    if (r3d::g_useGL) return "OpenGL 1.1";
    return "software";
}
