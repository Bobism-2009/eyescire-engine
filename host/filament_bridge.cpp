/* Filament 1.78 view for Eyescire. Built with MSVC and loaded by engine.dll.
   Lighting, shadows, sky, materials, and textures go through Filament.
   The editor's 2D chrome is a transparent overlay on top of this swap chain. */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "filament_bridge.h"

#include <filament/Camera.h>
#include <filament/Color.h>
#include <filament/Engine.h>
#include <filament/Fence.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <filamat/MaterialBuilder.h>

#include <geometry/SurfaceOrientation.h>

#include <utils/EntityManager.h>

#include <math/mat4.h>
#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace filament;
using namespace filament::math;

namespace {

struct GpuMesh {
    VertexBuffer* vb = nullptr;
    IndexBuffer* ib = nullptr;
    uint32_t count = 0;
};

struct TexRec {
    Texture* tex = nullptr;
    int w = 0, h = 0;
};

struct LineRec {
    float a[3], b[3];
    uint8_t r, g, bcol, alpha;
    int depth;
};

struct App {
    Engine* engine = nullptr;
    SwapChain* swap = nullptr;
    Renderer* renderer = nullptr;
    View* view = nullptr;
    Scene* scene = nullptr;
    Camera* camera = nullptr;
    utils::Entity cameraEntity;

    Material* lit = nullptr;
    Material* litFade = nullptr;
    Material* tex = nullptr;
    Material* texFade = nullptr;
    Material* unlit = nullptr;
    Material* unlitFront = nullptr;
    Material* sprite = nullptr;
    Material* overlayMat = nullptr;

    Texture* white = nullptr;
    Texture* flatN = nullptr;
    Texture* black = nullptr;
    TextureSampler samp;
    TextureSampler sampEdge;

    GpuMesh unit[4];
    std::unordered_map<const void*, GpuMesh> models;
    std::unordered_map<const void*, TexRec> textures;

    Skybox* sky = nullptr;
    Texture* skyTex = nullptr;
    std::string skyStamp;
    IndirectLight* ibl = nullptr;

    std::vector<utils::Entity> ents;
    std::vector<MaterialInstance*> mats;
    std::vector<VertexBuffer*> dynVb;
    std::vector<IndexBuffer*> dynIb;
    std::vector<Texture*> junkTex;
    std::vector<LineRec> lines;

    Texture* ovTex = nullptr;
    int ovW = 0, ovH = 0;
    int haveOverlay = 0;

    std::vector<unsigned char> capture;
    int capW = 0, capH = 0;
    int capReady = 0;

    int shadows = 1;
    int width = 16, height = 16;
    float clearR = 0, clearG = 0, clearB = 0;
};

App* A = nullptr;
std::string g_err;

void set_err(const char* s) { g_err = s ? s : ""; }

void free_buf(void*, size_t, void* user) {
    delete static_cast<std::vector<unsigned char>*>(user);
}

void note(const char* s) {
    OutputDebugStringA(s);
    OutputDebugStringA("\n");
}

Material* make_mat(const char* name, Shading shading, BlendingMode blend,
                   bool depthWrite, bool depthCull, VertexDomain domain,
                   bool uv, bool sampler, const char* code) {
    filamat::MaterialBuilder b;
    b.name(name)
        .shading(shading)
        .blending(blend)
        .culling(backend::CullingMode::NONE)
        .doubleSided(true)
        .depthWrite(depthWrite)
        .depthCulling(depthCull)
        .vertexDomain(domain)
        .platform(filamat::MaterialBuilder::Platform::DESKTOP)
        .targetApi(filamat::MaterialBuilder::TargetApi::OPENGL)
        .optimization(filamat::MaterialBuilder::Optimization::NONE)
        .material(code);
    if (uv) b.require(VertexAttribute::UV0);
    if (sampler && shading != Shading::LIT)
        b.parameter("image", backend::SamplerType::SAMPLER_2D);
    if (shading == Shading::LIT && !sampler) {
        b.parameter("baseColor", backend::UniformType::FLOAT4);
        b.parameter("roughness", backend::UniformType::FLOAT);
        b.parameter("metallic", backend::UniformType::FLOAT);
        b.parameter("reflectance", backend::UniformType::FLOAT);
    } else if (shading == Shading::LIT && sampler) {
        b.parameter("albedo", backend::SamplerType::SAMPLER_2D);
        b.parameter("normalMap", backend::SamplerType::SAMPLER_2D);
        b.parameter("roughnessMap", backend::SamplerType::SAMPLER_2D);
        b.parameter("metallicMap", backend::SamplerType::SAMPLER_2D);
        b.parameter("baseColor", backend::UniformType::FLOAT4);
        b.parameter("roughness", backend::UniformType::FLOAT);
        b.parameter("metallic", backend::UniformType::FLOAT);
        b.parameter("useNormal", backend::UniformType::FLOAT);
        b.parameter("useRough", backend::UniformType::FLOAT);
        b.parameter("useMetal", backend::UniformType::FLOAT);
    } else if (!sampler) {
        b.parameter("color", backend::UniformType::FLOAT4);
    } else if (std::strcmp(name, "EyescireSprite") == 0) {
        b.parameter("color", backend::UniformType::FLOAT4);
    }
    filamat::Package pkg = b.build(A->engine->getJobSystem());
    if (!pkg.isValid() || pkg.getSize() == 0) return nullptr;
    return Material::Builder().package(pkg.getData(), pkg.getSize()).build(*A->engine);
}

Texture* solid_tex(unsigned char r, unsigned char g, unsigned char b, bool srgb) {
    auto* bytes = new std::vector<unsigned char>{ r, g, b, 255 };
    Texture* t = Texture::Builder()
        .width(1).height(1).levels(1)
        .sampler(Texture::Sampler::SAMPLER_2D)
        .format(srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8)
        .build(*A->engine);
    if (!t) { delete bytes; return nullptr; }
    t->setImage(*A->engine, 0, Texture::PixelBufferDescriptor(
        bytes->data(), bytes->size(),
        Texture::Format::RGBA, Texture::Type::UBYTE,
        free_buf, bytes));
    return t;
}

mat4f mat_from(const float* m) {
    if (!m) return mat4f();
    mat4f M(mat4f::NO_INIT);
    M[0] = float4{ m[0], m[1], m[2], m[3] };
    M[1] = float4{ m[4], m[5], m[6], m[7] };
    M[2] = float4{ m[8], m[9], m[10], m[11] };
    M[3] = float4{ m[12], m[13], m[14], m[15] };
    return M;
}

float3 norm3(float x, float y, float z) {
    float l = std::sqrt(x * x + y * y + z * z);
    if (l < 1e-8f) return float3{ 0, 0, 1 };
    return float3{ x / l, y / l, z / l };
}

void build_tangents(const float* pos, const float* nrm, const float* uv, int count,
                    std::vector<quatf>& quats) {
    quats.assign((size_t)count, quatf(1, 0, 0, 0));
    if (!pos || !nrm || count < 1) return;
    std::vector<float3> P((size_t)count), N((size_t)count);
    std::vector<float2> U;
    for (int i = 0; i < count; ++i) {
        P[(size_t)i] = float3{ pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2] };
        N[(size_t)i] = norm3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]);
    }
    geometry::SurfaceOrientation::Builder b;
    b.vertexCount((size_t)count).normals(N.data()).positions(P.data());
    std::vector<uint3> tris;
    if (uv && count >= 3 && count % 3 == 0) {
        U.resize((size_t)count);
        for (int i = 0; i < count; ++i) U[(size_t)i] = float2{ uv[i * 2], uv[i * 2 + 1] };
        tris.resize((size_t)count / 3);
        for (size_t i = 0; i < tris.size(); ++i)
            tris[i] = uint3{ (uint32_t)(i * 3), (uint32_t)(i * 3 + 1), (uint32_t)(i * 3 + 2) };
        b.uvs(U.data()).triangleCount(tris.size()).triangles(tris.data());
    }
    geometry::SurfaceOrientation* so = b.build();
    if (!so) return;
    so->getQuats(quats.data(), (size_t)count);
    delete so;
}

IndexBuffer* make_ib(uint32_t count) {
    std::vector<uint32_t> idx(count);
    for (uint32_t i = 0; i < count; ++i) idx[i] = i;
    auto* heap = new std::vector<unsigned char>((unsigned char*)idx.data(),
        (unsigned char*)idx.data() + idx.size() * sizeof(uint32_t));
    IndexBuffer* ib = IndexBuffer::Builder()
        .indexCount(count)
        .bufferType(IndexBuffer::IndexType::UINT)
        .build(*A->engine);
    if (!ib) { delete heap; return nullptr; }
    ib->setBuffer(*A->engine, IndexBuffer::BufferDescriptor(
        heap->data(), heap->size(), free_buf, heap));
    return ib;
}

VertexBuffer* make_vb(const float* pos, const quatf* quat, const float* uv, int count, bool needUv) {
    struct V { float p[3]; float q[4]; float u, v; };
    auto* heap = new std::vector<unsigned char>((size_t)count * sizeof(V));
    auto* v = reinterpret_cast<V*>(heap->data());
    for (int i = 0; i < count; ++i) {
        v[i].p[0] = pos[i * 3];
        v[i].p[1] = pos[i * 3 + 1];
        v[i].p[2] = pos[i * 3 + 2];
        v[i].q[0] = quat[i].x;
        v[i].q[1] = quat[i].y;
        v[i].q[2] = quat[i].z;
        v[i].q[3] = quat[i].w;
        v[i].u = uv ? uv[i * 2] : 0;
        v[i].v = uv ? uv[i * 2 + 1] : 0;
    }
    VertexBuffer* vb = VertexBuffer::Builder()
        .vertexCount((uint32_t)count)
        .bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0, sizeof(V))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::FLOAT4, 12, sizeof(V))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 28, sizeof(V))
        .build(*A->engine);
    if (!vb) { delete heap; return nullptr; }
    (void)needUv;
    vb->setBufferAt(*A->engine, 0, VertexBuffer::BufferDescriptor(
        heap->data(), heap->size(), free_buf, heap));
    return vb;
}

VertexBuffer* make_pos_vb(const float* pos, int count) {
    auto* heap = new std::vector<unsigned char>((unsigned char*)pos,
        (unsigned char*)pos + (size_t)count * 12);
    VertexBuffer* vb = VertexBuffer::Builder()
        .vertexCount((uint32_t)count)
        .bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0, 12)
        .build(*A->engine);
    if (!vb) { delete heap; return nullptr; }
    vb->setBufferAt(*A->engine, 0, VertexBuffer::BufferDescriptor(
        heap->data(), heap->size(), free_buf, heap));
    return vb;
}

void track_entity(utils::Entity e) { A->ents.push_back(e); }

void add_mesh(VertexBuffer* vb, IndexBuffer* ib, uint32_t count, const mat4f& xf,
              MaterialInstance* mi, int castShadow, int priority, bool keep) {
    auto& em = utils::EntityManager::get();
    utils::Entity e = em.create();
    A->engine->getTransformManager().create(e, {}, xf);
    RenderableManager::Builder(1)
        .boundingBox({ { -1, -1, -1 }, { 1, 1, 1 } })
        .material(0, mi)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vb, ib, 0, count)
        .culling(false)
        .castShadows(castShadow != 0)
        .receiveShadows(true)
        .priority((uint8_t)priority)
        .build(*A->engine, e);
    A->scene->addEntity(e);
    track_entity(e);
    A->mats.push_back(mi);
    if (!keep) {
        A->dynVb.push_back(vb);
        A->dynIb.push_back(ib);
    }
}

MaterialInstance* lit_mi(int r, int g, int b, int a, int textured) {
    bool fade = a < 250;
    Material* mat = textured ? (fade ? A->texFade : A->tex) : (fade ? A->litFade : A->lit);
    MaterialInstance* mi = mat->createInstance();
    float4 c{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
    mi->setParameter("baseColor", RgbaType::sRGB, c);
    mi->setParameter("roughness", 0.72f);
    mi->setParameter("metallic", 0.0f);
    if (!textured) mi->setParameter("reflectance", 0.5f);
    else {
        mi->setParameter("useNormal", 0.0f);
        mi->setParameter("useRough", 0.0f);
        mi->setParameter("useMetal", 0.0f);
        mi->setParameter("albedo", A->white, A->samp);
        mi->setParameter("normalMap", A->flatN, A->samp);
        mi->setParameter("roughnessMap", A->white, A->samp);
        mi->setParameter("metallicMap", A->black, A->samp);
    }
    return mi;
}

MaterialInstance* unlit_mi(int r, int g, int b, int a, int front) {
    Material* mat = front ? A->unlitFront : A->unlit;
    MaterialInstance* mi = mat->createInstance();
    mi->setParameter("color", RgbaType::sRGB, float4{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f });
    return mi;
}

Texture* upload_rgb(const unsigned char* rgb, int w, int h, bool srgb, bool bottomUp) {
    if (!rgb || w < 1 || h < 1) return nullptr;
    auto* bytes = new std::vector<unsigned char>((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        int sy = bottomUp ? (h - 1 - y) : y;
        for (int x = 0; x < w; ++x) {
            const unsigned char* s = rgb + ((size_t)sy * w + x) * 3;
            unsigned char* d = bytes->data() + ((size_t)y * w + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
    }
    Texture* t = Texture::Builder()
        .width((uint32_t)w).height((uint32_t)h).levels(1)
        .sampler(Texture::Sampler::SAMPLER_2D)
        .format(srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8)
        .build(*A->engine);
    if (!t) { delete bytes; return nullptr; }
    t->setImage(*A->engine, 0, Texture::PixelBufferDescriptor(
        bytes->data(), bytes->size(),
        Texture::Format::RGBA, Texture::Type::UBYTE, free_buf, bytes));
    return t;
}

Texture* cached_rgb(const void* key, const unsigned char* rgb, int w, int h, bool srgb) {
    if (!key || !rgb) return nullptr;
    auto it = A->textures.find(key);
    if (it != A->textures.end() && it->second.w == w && it->second.h == h) return it->second.tex;
    Texture* t = upload_rgb(rgb, w, h, srgb, true);
    if (!t) return nullptr;
    if (it != A->textures.end() && it->second.tex) A->engine->destroy(it->second.tex);
    A->textures[key] = TexRec{ t, w, h };
    return t;
}

void destroy_frame() {
    if (!A || !A->engine) return;
    for (utils::Entity e : A->ents) {
        A->scene->remove(e);
        A->engine->destroy(e);
        utils::EntityManager::get().destroy(e);
    }
    A->ents.clear();
    for (MaterialInstance* mi : A->mats) A->engine->destroy(mi);
    A->mats.clear();
    for (VertexBuffer* vb : A->dynVb) A->engine->destroy(vb);
    A->dynVb.clear();
    for (IndexBuffer* ib : A->dynIb) A->engine->destroy(ib);
    A->dynIb.clear();
    A->lines.clear();
    A->haveOverlay = 0;
    for (Texture* t : A->junkTex) A->engine->destroy(t);
    A->junkTex.clear();
}

void destroy_mesh(GpuMesh& m) {
    if (!A || !A->engine) return;
    if (m.vb) A->engine->destroy(m.vb);
    if (m.ib) A->engine->destroy(m.ib);
    m = {};
}

bool cache_mesh(GpuMesh& dst, const float* pos, const float* nrm, const float* uv, int count) {
    if (dst.vb) return true;
    if (!pos || !nrm || count < 3) return false;
    std::vector<quatf> q;
    build_tangents(pos, nrm, uv, count, q);
    dst.vb = make_vb(pos, q.data(), uv, count, uv != nullptr);
    dst.ib = make_ib((uint32_t)count);
    dst.count = (uint32_t)count;
    if (!dst.vb || !dst.ib) {
        destroy_mesh(dst);
        return false;
    }
    return true;
}

void push9(std::vector<float>& v, float x, float y, float z, float nx, float ny, float nz) {
    float s[6] = { x, y, z, nx, ny, nz };
    v.insert(v.end(), s, s + 6);
}

void build_units() {
    std::vector<float> box, sphere, cyl;
    static const float F[6][4][3] = {
        { { .5f,-.5f,-.5f},{ .5f, .5f,-.5f},{ .5f, .5f, .5f},{ .5f,-.5f, .5f} },
        { {-.5f,-.5f, .5f},{-.5f, .5f, .5f},{-.5f, .5f,-.5f},{-.5f,-.5f,-.5f} },
        { {-.5f, .5f,-.5f},{-.5f, .5f, .5f},{ .5f, .5f, .5f},{ .5f, .5f,-.5f} },
        { {-.5f,-.5f, .5f},{-.5f,-.5f,-.5f},{ .5f,-.5f,-.5f},{ .5f,-.5f, .5f} },
        { {-.5f,-.5f, .5f},{ .5f,-.5f, .5f},{ .5f, .5f, .5f},{-.5f, .5f, .5f} },
        { { .5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{-.5f, .5f,-.5f},{ .5f, .5f,-.5f} },
    };
    static const float N[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    auto tri = [&](std::vector<float>& dst, const float* a, const float* b, const float* c) {
        push9(dst, a[0], a[1], a[2], a[3], a[4], a[5]);
        push9(dst, b[0], b[1], b[2], b[3], b[4], b[5]);
        push9(dst, c[0], c[1], c[2], c[3], c[4], c[5]);
    };
    for (int f = 0; f < 6; ++f) {
        float q[4][6];
        for (int i = 0; i < 4; ++i) {
            q[i][0] = F[f][i][0]; q[i][1] = F[f][i][1]; q[i][2] = F[f][i][2];
            q[i][3] = N[f][0]; q[i][4] = N[f][1]; q[i][5] = N[f][2];
        }
        tri(box, q[0], q[1], q[2]);
        tri(box, q[0], q[2], q[3]);
    }
    const int SEG = 32, RING = 16;
    for (int j = 0; j < RING; ++j) {
        for (int i = 0; i < SEG; ++i) {
            float th0 = (float)j / RING * 3.14159265f;
            float th1 = (float)(j + 1) / RING * 3.14159265f;
            float ph0 = (float)i / SEG * 6.2831853f;
            float ph1 = (float)(i + 1) / SEG * 6.2831853f;
            auto at = [&](float th, float ph, float* o) {
                float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
                o[0] = x * .5f; o[1] = y * .5f; o[2] = z * .5f;
                o[3] = x; o[4] = y; o[5] = z;
            };
            float a[6], b[6], c[6], d[6];
            at(th0, ph0, a); at(th0, ph1, b); at(th1, ph1, c); at(th1, ph0, d);
            tri(sphere, a, b, c);
            tri(sphere, a, c, d);
        }
    }
    const int CS = 40;
    for (int i = 0; i < CS; ++i) {
        float a0 = (float)i / CS * 6.2831853f;
        float a1 = (float)(i + 1) / CS * 6.2831853f;
        float x0 = std::cos(a0), z0 = std::sin(a0), x1 = std::cos(a1), z1 = std::sin(a1);
        float p0[6] = { x0 * .5f, .5f, z0 * .5f, x0, 0, z0 };
        float p1[6] = { x0 * .5f, -.5f, z0 * .5f, x0, 0, z0 };
        float p2[6] = { x1 * .5f, .5f, z1 * .5f, x1, 0, z1 };
        float p3[6] = { x1 * .5f, -.5f, z1 * .5f, x1, 0, z1 };
        tri(cyl, p0, p2, p3);
        tri(cyl, p0, p3, p1);
    }
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap == 0 ? .5f : -.5f;
        float ny = cap == 0 ? 1.f : -1.f;
        float center[6] = { 0, y, 0, 0, ny, 0 };
        for (int i = 0; i < CS; ++i) {
            float a0 = (float)i / CS * 6.2831853f;
            float a1 = (float)(i + 1) / CS * 6.2831853f;
            float p0[6] = { std::cos(a0) * .5f, y, std::sin(a0) * .5f, 0, ny, 0 };
            float p1[6] = { std::cos(a1) * .5f, y, std::sin(a1) * .5f, 0, ny, 0 };
            if (cap == 0) tri(cyl, center, p0, p1);
            else tri(cyl, center, p1, p0);
        }
    }
    auto split = [](const std::vector<float>& src, std::vector<float>& p, std::vector<float>& n) {
        int nv = (int)src.size() / 6;
        p.resize((size_t)nv * 3);
        n.resize((size_t)nv * 3);
        for (int i = 0; i < nv; ++i) {
            p[i * 3] = src[i * 6]; p[i * 3 + 1] = src[i * 6 + 1]; p[i * 3 + 2] = src[i * 6 + 2];
            n[i * 3] = src[i * 6 + 3]; n[i * 3 + 1] = src[i * 6 + 4]; n[i * 3 + 2] = src[i * 6 + 5];
        }
    };
    std::vector<float> p, n;
    split(box, p, n); cache_mesh(A->unit[1], p.data(), n.data(), nullptr, (int)p.size() / 3);
    split(sphere, p, n); cache_mesh(A->unit[2], p.data(), n.data(), nullptr, (int)p.size() / 3);
    split(cyl, p, n); cache_mesh(A->unit[3], p.data(), n.data(), nullptr, (int)p.size() / 3);
}

void flush_lines() {
    if (A->lines.empty() || !A->unlit) return;
    std::unordered_map<uint32_t, std::vector<float>> groups;
    for (const LineRec& L : A->lines) {
        uint32_t key = ((uint32_t)L.r) | ((uint32_t)L.g << 8) | ((uint32_t)L.bcol << 16)
            | ((uint32_t)L.alpha << 24) | (L.depth ? 0x80000000u : 0);
        auto& v = groups[key];
        v.insert(v.end(), L.a, L.a + 3);
        v.insert(v.end(), L.b, L.b + 3);
    }
    for (auto& kv : groups) {
        const std::vector<float>& pos = kv.second;
        int n = (int)pos.size() / 3;
        if (n < 2) continue;
        VertexBuffer* vb = make_pos_vb(pos.data(), n);
        IndexBuffer* ib = make_ib((uint32_t)n);
        if (!vb || !ib) {
            if (vb) A->engine->destroy(vb);
            if (ib) A->engine->destroy(ib);
            continue;
        }
        uint32_t key = kv.first;
        int front = (key & 0x80000000u) ? 0 : 1;
        MaterialInstance* mi = unlit_mi(key & 255, (key >> 8) & 255, (key >> 16) & 255, (key >> 24) & 255, front);
        auto& em = utils::EntityManager::get();
        utils::Entity e = em.create();
        A->engine->getTransformManager().create(e, {}, mat4f());
        RenderableManager::Builder(1)
            .boundingBox({ { -1, -1, -1 }, { 1, 1, 1 } })
            .material(0, mi)
            .geometry(0, RenderableManager::PrimitiveType::LINES, vb, ib, 0, (uint32_t)n)
            .culling(false)
            .castShadows(false)
            .receiveShadows(false)
            .priority(front ? 7 : 6)
            .build(*A->engine, e);
        A->scene->addEntity(e);
        track_entity(e);
        A->mats.push_back(mi);
        A->dynVb.push_back(vb);
        A->dynIb.push_back(ib);
    }
    A->lines.clear();
}

void draw_overlay() {
    if (!A->haveOverlay || !A->ovTex || !A->overlayMat) return;
    float pos[18] = {
        -1, -1, 0,  1, -1, 0,  1, 1, 0,
        -1, -1, 0,  1, 1, 0,  -1, 1, 0
    };
    float uv[12] = { 0, 1,  1, 1,  1, 0,  0, 1,  1, 0,  0, 0 };
    struct V { float p[3]; float u, v; };
    auto* heap = new std::vector<unsigned char>(6 * sizeof(V));
    auto* v = reinterpret_cast<V*>(heap->data());
    for (int i = 0; i < 6; ++i) {
        v[i].p[0] = pos[i * 3]; v[i].p[1] = pos[i * 3 + 1]; v[i].p[2] = pos[i * 3 + 2];
        v[i].u = uv[i * 2]; v[i].v = uv[i * 2 + 1];
    }
    VertexBuffer* vb = VertexBuffer::Builder()
        .vertexCount(6).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0, sizeof(V))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 12, sizeof(V))
        .build(*A->engine);
    IndexBuffer* ib = make_ib(6);
    if (!vb || !ib) {
        delete heap;
        if (vb) A->engine->destroy(vb);
        if (ib) A->engine->destroy(ib);
        return;
    }
    vb->setBufferAt(*A->engine, 0, VertexBuffer::BufferDescriptor(
        heap->data(), heap->size(), free_buf, heap));
    MaterialInstance* mi = A->overlayMat->createInstance();
    mi->setParameter("image", A->ovTex, A->sampEdge);
    auto& em = utils::EntityManager::get();
    utils::Entity e = em.create();
    A->engine->getTransformManager().create(e, {}, mat4f());
    RenderableManager::Builder(1)
        .boundingBox({ { -1, -1, -1 }, { 1, 1, 1 } })
        .material(0, mi)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vb, ib, 0, 6)
        .culling(false)
        .castShadows(false)
        .receiveShadows(false)
        .priority(7)
        .build(*A->engine, e);
    A->scene->addEntity(e);
    track_entity(e);
    A->mats.push_back(mi);
    A->dynVb.push_back(vb);
    A->dynIb.push_back(ib);
}

float lin(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

extern "C" const char* fb_last_error(void) { return g_err.c_str(); }
extern "C" int fb_ok(void) { return A && A->engine ? 1 : 0; }

extern "C" int fb_init(void* hwnd) {
    if (A) fb_shutdown();
    g_err.clear();
    if (!hwnd) { set_err("no window"); return 0; }
    A = new App();
    A->samp = TextureSampler(TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);
    A->sampEdge = TextureSampler(TextureSampler::MagFilter::NEAREST, TextureSampler::WrapMode::CLAMP_TO_EDGE);
    try {
        filamat::MaterialBuilder::init();
        A->engine = Engine::create();
        if (!A->engine) { set_err("Engine::create failed"); fb_shutdown(); return 0; }
        A->swap = A->engine->createSwapChain(hwnd, SwapChain::CONFIG_READABLE);
        A->renderer = A->engine->createRenderer();
        A->scene = A->engine->createScene();
        A->view = A->engine->createView();
        A->cameraEntity = utils::EntityManager::get().create();
        A->camera = A->engine->createCamera(A->cameraEntity);
        A->view->setCamera(A->camera);
        A->view->setScene(A->scene);
        A->view->setPostProcessingEnabled(true);
        A->view->setShadowingEnabled(true);
        Renderer::ClearOptions clear;
        clear.clear = true;
        clear.clearColor = { 0, 0, 0, 1 };
        A->renderer->setClearOptions(clear);

        const char* litCode =
            "void material(inout MaterialInputs material) {"
            "  prepareMaterial(material);"
            "  material.baseColor = materialParams.baseColor;"
            "  material.roughness = materialParams.roughness;"
            "  material.metallic = materialParams.metallic;"
            "  material.reflectance = materialParams.reflectance;"
            "}";
        const char* texCode =
            "void material(inout MaterialInputs material) {"
            "  if (materialParams.useNormal > 0.5) {"
            "    material.normal = texture(materialParams_normalMap, getUV0()).xyz * 2.0 - 1.0;"
            "  }"
            "  prepareMaterial(material);"
            "  material.baseColor = texture(materialParams_albedo, getUV0()) * materialParams.baseColor;"
            "  material.roughness = materialParams.useRough > 0.5"
            "    ? texture(materialParams_roughnessMap, getUV0()).r : materialParams.roughness;"
            "  material.metallic = materialParams.useMetal > 0.5"
            "    ? texture(materialParams_metallicMap, getUV0()).r : materialParams.metallic;"
            "  material.reflectance = 0.5;"
            "}";
        const char* unlitCode =
            "void material(inout MaterialInputs material) {"
            "  prepareMaterial(material);"
            "  material.baseColor = materialParams.color;"
            "}";
        const char* spriteCode =
            "void material(inout MaterialInputs material) {"
            "  prepareMaterial(material);"
            "  material.baseColor = texture(materialParams_image, getUV0()) * materialParams.color;"
            "}";
        const char* overCode =
            "void material(inout MaterialInputs material) {"
            "  prepareMaterial(material);"
            "  material.baseColor = texture(materialParams_image, getUV0());"
            "}";
        A->lit = make_mat("EyescireLit", Shading::LIT, BlendingMode::OPAQUE, true, true, VertexDomain::OBJECT, false, false, litCode);
        A->litFade = make_mat("EyescireLitFade", Shading::LIT, BlendingMode::TRANSPARENT, false, true, VertexDomain::OBJECT, false, false, litCode);
        A->tex = make_mat("EyescireTex", Shading::LIT, BlendingMode::OPAQUE, true, true, VertexDomain::OBJECT, true, true, texCode);
        A->texFade = make_mat("EyescireTexFade", Shading::LIT, BlendingMode::TRANSPARENT, false, true, VertexDomain::OBJECT, true, true, texCode);
        A->unlit = make_mat("EyescireUnlit", Shading::UNLIT, BlendingMode::TRANSPARENT, false, true, VertexDomain::OBJECT, false, false, unlitCode);
        A->unlitFront = make_mat("EyescireFront", Shading::UNLIT, BlendingMode::TRANSPARENT, false, false, VertexDomain::OBJECT, false, false, unlitCode);
        A->sprite = make_mat("EyescireSprite", Shading::UNLIT, BlendingMode::TRANSPARENT, false, true, VertexDomain::OBJECT, true, true, spriteCode);
        A->overlayMat = make_mat("EyescireOverlay", Shading::UNLIT, BlendingMode::TRANSPARENT, false, false, VertexDomain::DEVICE, true, true, overCode);
        if (!A->lit || !A->litFade || !A->tex || !A->texFade || !A->unlit || !A->unlitFront || !A->sprite || !A->overlayMat) {
            set_err("material compile failed");
            fb_shutdown();
            return 0;
        }
        A->white = solid_tex(255, 255, 255, true);
        A->black = solid_tex(0, 0, 0, false);
        A->flatN = solid_tex(128, 128, 255, false);
        if (!A->white || !A->black || !A->flatN) {
            set_err("default textures failed");
            fb_shutdown();
            return 0;
        }
        build_units();
        if (!A->unit[1].vb || !A->unit[2].vb || !A->unit[3].vb) {
            set_err("unit meshes failed");
            fb_shutdown();
            return 0;
        }
        note("filament bridge ready");
        return 1;
    } catch (const std::exception& ex) {
        set_err(ex.what());
        fb_shutdown();
        return 0;
    } catch (...) {
        set_err("filament init failed");
        fb_shutdown();
        return 0;
    }
}

extern "C" void fb_shutdown(void) {
    if (!A) return;
    if (A->engine) {
        destroy_frame();
        for (int i = 1; i <= 3; ++i) destroy_mesh(A->unit[i]);
        for (auto& kv : A->models) destroy_mesh(kv.second);
        A->models.clear();
        for (auto& kv : A->textures) if (kv.second.tex) A->engine->destroy(kv.second.tex);
        A->textures.clear();
        if (A->sky) { A->scene->setSkybox(nullptr); A->engine->destroy(A->sky); A->sky = nullptr; }
        if (A->skyTex) { A->engine->destroy(A->skyTex); A->skyTex = nullptr; }
        if (A->ibl) { A->scene->setIndirectLight(nullptr); A->engine->destroy(A->ibl); A->ibl = nullptr; }
        if (A->ovTex) { A->engine->destroy(A->ovTex); A->ovTex = nullptr; }
        Material* ms[] = { A->lit, A->litFade, A->tex, A->texFade, A->unlit, A->unlitFront, A->sprite, A->overlayMat, nullptr };
        for (int i = 0; ms[i]; ++i) A->engine->destroy(ms[i]);
        Texture* ts[] = { A->white, A->flatN, A->black, nullptr };
        for (int i = 0; ts[i]; ++i) A->engine->destroy(ts[i]);
        if (A->camera) A->engine->destroyCameraComponent(A->cameraEntity);
        utils::EntityManager::get().destroy(A->cameraEntity);
        if (A->view) A->engine->destroy(A->view);
        if (A->scene) A->engine->destroy(A->scene);
        if (A->renderer) A->engine->destroy(A->renderer);
        if (A->swap) A->engine->destroy(A->swap);
        A->engine->flushAndWait();
        Engine::destroy(&A->engine);
        filamat::MaterialBuilder::shutdown();
    }
    delete A;
    A = nullptr;
}

extern "C" void fb_begin(float ex, float ey, float ez,
                         float tx, float ty, float tz,
                         float ux, float uy, float uz,
                         float fov_deg, float aspect,
                         int w, int h,
                         int br, int bg, int bb,
                         int aa, int shadows,
                         int fog, int fog_r, int fog_g, int fog_b, float fog_dist) {
    if (!A || !A->engine) return;
    destroy_frame();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    A->width = w;
    A->height = h;
    A->shadows = shadows ? 1 : 0;
    A->clearR = lin(br / 255.0f);
    A->clearG = lin(bg / 255.0f);
    A->clearB = lin(bb / 255.0f);
    if (aspect < 0.01f) aspect = (float)w / (float)h;
    double3 eye{ ex, ey, ez };
    double3 target{ tx, ty, tz };
    double3 up{ ux, uy, uz };
    if (length(target - eye) < 1e-4) target = eye + double3{ 0, 0, -1 };
    if (length(up) < 1e-4) up = double3{ 0, 1, 0 };
    /* f/8 keeps a sun of a few tens of thousands of lux in the same range
       the editor's colors were authored for. f/16 left the view nearly black. */
    A->camera->setExposure(8.0f, 1.0f / 125.0f, 100.0f);
    A->camera->setProjection(fov_deg, aspect, 0.05, 4000.0, Camera::Fov::VERTICAL);
    A->camera->lookAt(eye, target, up);
    A->view->setViewport({ 0, 0, (uint32_t)w, (uint32_t)h });
    A->view->setShadowingEnabled(shadows != 0);
    MultiSampleAntiAliasingOptions msaa;
    msaa.enabled = aa != 0;
    msaa.sampleCount = 4;
    A->view->setMultiSampleAntiAliasingOptions(msaa);
    A->view->setAntiAliasing(AntiAliasing::NONE);
    FogOptions fo;
    fo.enabled = fog && fog_dist > 0.01f;
    fo.distance = fog_dist * 0.25f;
    fo.maximumOpacity = 1.0f;
    fo.heightFalloff = 0.0f;
    fo.density = fog_dist > 0.01f ? 2.2f / fog_dist : 0.0f;
    fo.color = { lin(fog_r / 255.0f), lin(fog_g / 255.0f), lin(fog_b / 255.0f) };
    A->view->setFogOptions(fo);
    float lr = A->clearR, lg = A->clearG, lb = A->clearB;
    if (A->sky && A->skyTex) {
        /* A cubemap sky replaces the flat clear. */
    } else {
        if (A->sky) { A->scene->setSkybox(nullptr); A->engine->destroy(A->sky); A->sky = nullptr; }
        A->sky = Skybox::Builder().color({ lr, lg, lb, 1 }).build(*A->engine);
        A->scene->setSkybox(A->sky);
    }
}

extern "C" void fb_sun(float dx, float dy, float dz, float r, float g, float b, float lux, int shadows) {
    if (!A || !A->engine) return;
    float3 d = norm3(dx, dy, dz);
    auto& em = utils::EntityManager::get();
    utils::Entity e = em.create();
    LightManager::ShadowOptions so;
    so.mapSize = 2048;
    so.shadowNearHint = 0.05f;
    so.shadowFarHint = 80.0f;
    bool cast = shadows && A->shadows;
    LightManager::Builder(cast ? LightManager::Type::SUN : LightManager::Type::DIRECTIONAL)
        .color(LinearColor{ r, g, b })
        .intensity(lux > 1.f ? lux : 1.f)
        .direction(d)
        .castShadows(cast)
        .shadowOptions(so)
        .build(*A->engine, e);
    A->scene->addEntity(e);
    track_entity(e);
}

extern "C" void fb_point(float x, float y, float z, float r, float g, float b, float candela, float radius) {
    if (!A || !A->engine) return;
    if (radius < 0.05f) radius = 0.05f;
    auto& em = utils::EntityManager::get();
    utils::Entity e = em.create();
    LightManager::Builder(LightManager::Type::POINT)
        .color(LinearColor{ r, g, b })
        .intensityCandela(candela > 1.f ? candela : 1.f)
        .position(float3{ x, y, z })
        .falloff(radius)
        .castShadows(false)
        .build(*A->engine, e);
    A->scene->addEntity(e);
    track_entity(e);
}

extern "C" void fb_spot(float x, float y, float z, float dx, float dy, float dz,
                        float r, float g, float b, float candela, float radius,
                        float inner_rad, float outer_rad) {
    if (!A || !A->engine) return;
    if (radius < 0.05f) radius = 0.05f;
    if (outer_rad < inner_rad) outer_rad = inner_rad;
    auto& em = utils::EntityManager::get();
    utils::Entity e = em.create();
    LightManager::Builder(LightManager::Type::SPOT)
        .color(LinearColor{ r, g, b })
        .intensityCandela(candela > 1.f ? candela : 1.f)
        .position(float3{ x, y, z })
        .direction(norm3(dx, dy, dz))
        .falloff(radius)
        .spotLightCone(inner_rad, outer_rad)
        .castShadows(false)
        .build(*A->engine, e);
    A->scene->addEntity(e);
    track_entity(e);
}

extern "C" void fb_ambient(float r, float g, float b) {
    if (!A || !A->engine) return;
    float3 sh[1] = { float3{ r, g, b } * 3.14159265f };
    if (A->ibl) {
        A->scene->setIndirectLight(nullptr);
        A->engine->destroy(A->ibl);
        A->ibl = nullptr;
    }
    A->ibl = IndirectLight::Builder().irradiance(1, sh).intensity(1.0f).build(*A->engine);
    A->scene->setIndirectLight(A->ibl);
}

extern "C" void fb_sky(const unsigned char* const faces[6], const int wh[12], const char* stamp) {
    if (!A || !A->engine) return;
    if (!faces || !wh) {
        if (!A->skyTex) return;
        if (A->sky) { A->scene->setSkybox(nullptr); A->engine->destroy(A->sky); A->sky = nullptr; }
        A->engine->destroy(A->skyTex);
        A->skyTex = nullptr;
        A->skyStamp.clear();
        A->sky = Skybox::Builder().color({ A->clearR, A->clearG, A->clearB, 1 }).build(*A->engine);
        A->scene->setSkybox(A->sky);
        return;
    }
    std::string mark = stamp ? stamp : "";
    if (A->skyTex && mark == A->skyStamp) return;
    int w = wh[0], h = wh[1];
    if (w < 1 || h < 1) return;
    for (int i = 0; i < 6; ++i) {
        if (!faces[i] || wh[i * 2] != w || wh[i * 2 + 1] != h) return;
    }
    Texture* tex = Texture::Builder()
        .width((uint32_t)w).height((uint32_t)h).levels(1)
        .sampler(Texture::Sampler::SAMPLER_CUBEMAP)
        .format(Texture::InternalFormat::SRGB8)
        .build(*A->engine);
    if (!tex) return;
    for (int face = 0; face < 6; ++face) {
        auto* bytes = new std::vector<unsigned char>((size_t)w * h * 3);
        for (int y = 0; y < h; ++y) {
            const unsigned char* src = faces[face] + (size_t)(h - 1 - y) * w * 3;
            std::memcpy(bytes->data() + (size_t)y * w * 3, src, (size_t)w * 3);
        }
        tex->setImage(*A->engine, 0, 0, 0, (uint32_t)face, (uint32_t)w, (uint32_t)h, 1,
            Texture::PixelBufferDescriptor(bytes->data(), bytes->size(),
                Texture::Format::RGB, Texture::Type::UBYTE, free_buf, bytes));
    }
    Skybox* sky = Skybox::Builder().environment(tex).showSun(false).build(*A->engine);
    if (!sky) { A->engine->destroy(tex); return; }
    if (A->sky) { A->scene->setSkybox(nullptr); A->engine->destroy(A->sky); }
    if (A->skyTex) A->engine->destroy(A->skyTex);
    A->sky = sky;
    A->skyTex = tex;
    A->skyStamp = mark;
    A->scene->setSkybox(sky);
}

extern "C" void fb_prim(int kind, const float xform[16], int r, int g, int b, int a,
                        int cast_shadow, int mode) {
    if (!A || !A->engine || kind < 1 || kind > 3 || !A->unit[kind].vb) return;
    if (a < 8) return;
    /* mode 2 is a selection hull: depth-tested, no depth write, drawn before
       the solid mesh so only the rim survives. mode 1 is an unlit depth-tested mesh. */
    MaterialInstance* mi = mode == 0 ? lit_mi(r, g, b, a, 0) : unlit_mi(r, g, b, a, 0);
    int cast = (mode == 0 && a >= 250) ? cast_shadow : 0;
    int pri = mode == 2 ? 3 : mode == 0 ? (a < 250 ? 5 : 4) : 6;
    add_mesh(A->unit[kind].vb, A->unit[kind].ib, A->unit[kind].count, mat_from(xform), mi, cast, pri, true);
}

extern "C" void fb_model(const void* key,
                         const float* pos, const float* nrm, const float* uv, int count,
                         const float xform[16], int r, int g, int b, int a, int cast_shadow,
                         const unsigned char* albedo, int aw, int ah,
                         const unsigned char* normal, int nw, int nh,
                         const unsigned char* metal, int mw, int mh,
                         const unsigned char* rough, int rw, int rh) {
    if (!A || !A->engine || !key || count < 3 || a < 8) return;
    GpuMesh& mesh = A->models[key];
    if (!cache_mesh(mesh, pos, nrm, uv, count)) return;
    bool mapped = albedo && aw > 0 && ah > 0;
    MaterialInstance* mi = lit_mi(r, g, b, a, mapped ? 1 : 0);
    if (mapped) {
        Texture* al = cached_rgb(albedo, albedo, aw, ah, true);
        if (al) mi->setParameter("albedo", al, A->samp);
        if (normal && nw > 0 && nh > 0) {
            Texture* nt = cached_rgb(normal, normal, nw, nh, false);
            if (nt) { mi->setParameter("normalMap", nt, A->samp); mi->setParameter("useNormal", 1.0f); }
        }
        if (rough && rw > 0 && rh > 0) {
            Texture* rt = cached_rgb(rough, rough, rw, rh, false);
            if (rt) { mi->setParameter("roughnessMap", rt, A->samp); mi->setParameter("useRough", 1.0f); }
        }
        if (metal && mw > 0 && mh > 0) {
            Texture* mt = cached_rgb(metal, metal, mw, mh, false);
            if (mt) { mi->setParameter("metallicMap", mt, A->samp); mi->setParameter("useMetal", 1.0f); }
        }
    }
    int cast = a >= 250 ? cast_shadow : 0;
    add_mesh(mesh.vb, mesh.ib, mesh.count, mat_from(xform), mi, cast, a < 250 ? 5 : 4, true);
}

extern "C" void fb_tris(const float* pos, const float* nrm, int count, const float xform[16],
                        int r, int g, int b, int a,
                        int lit, int depth_test, int depth_write, int cast_shadow) {
    if (!A || !A->engine || !pos || count < 3 || a < 1) return;
    (void)depth_write;
    if (lit && nrm) {
        std::vector<quatf> q;
        build_tangents(pos, nrm, nullptr, count, q);
        VertexBuffer* vb = make_vb(pos, q.data(), nullptr, count, false);
        IndexBuffer* ib = make_ib((uint32_t)count);
        if (!vb || !ib) {
            if (vb) A->engine->destroy(vb);
            if (ib) A->engine->destroy(ib);
            return;
        }
        MaterialInstance* mi = lit_mi(r, g, b, a, 0);
        add_mesh(vb, ib, (uint32_t)count, mat_from(xform), mi, a >= 250 ? cast_shadow : 0, a < 250 ? 5 : 4, false);
        return;
    }
    VertexBuffer* vb = make_pos_vb(pos, count);
    IndexBuffer* ib = make_ib((uint32_t)count);
    if (!vb || !ib) {
        if (vb) A->engine->destroy(vb);
        if (ib) A->engine->destroy(ib);
        return;
    }
    int front = depth_test ? 0 : 1;
    int pri = depth_test == 2 ? 3 : front ? 6 : 5;
    MaterialInstance* mi = unlit_mi(r, g, b, a, front);
    add_mesh(vb, ib, (uint32_t)count, mat_from(xform), mi, 0, pri, false);
}

extern "C" void fb_line(float x1, float y1, float z1, float x2, float y2, float z2,
                        int r, int g, int b, int a, int depth_test) {
    if (!A || a < 1) return;
    LineRec L;
    L.a[0] = x1; L.a[1] = y1; L.a[2] = z1;
    L.b[0] = x2; L.b[1] = y2; L.b[2] = z2;
    L.r = (uint8_t)r; L.g = (uint8_t)g; L.bcol = (uint8_t)b; L.alpha = (uint8_t)a;
    L.depth = depth_test ? 1 : 0;
    A->lines.push_back(L);
}

extern "C" void fb_sprite(const void* key, const unsigned char* rgba, int tw, int th,
                          const float pos[12], const float uv[8],
                          int r, int g, int b, int a) {
    if (!A || !A->engine || !key || !rgba || !pos || !uv || tw < 1 || th < 1 || a < 1) return;
    Texture* tex = nullptr;
    auto it = A->textures.find(key);
    if (it != A->textures.end() && it->second.w == tw && it->second.h == th) tex = it->second.tex;
    else {
        auto* bytes = new std::vector<unsigned char>(rgba, rgba + (size_t)tw * th * 4);
        tex = Texture::Builder()
            .width((uint32_t)tw).height((uint32_t)th).levels(1)
            .sampler(Texture::Sampler::SAMPLER_2D)
            .format(Texture::InternalFormat::SRGB8_A8)
            .build(*A->engine);
        if (!tex) { delete bytes; return; }
        tex->setImage(*A->engine, 0, Texture::PixelBufferDescriptor(
            bytes->data(), bytes->size(),
            Texture::Format::RGBA, Texture::Type::UBYTE, free_buf, bytes));
        if (it != A->textures.end() && it->second.tex) A->engine->destroy(it->second.tex);
        A->textures[key] = TexRec{ tex, tw, th };
    }
    float triP[18], triU[12];
    int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; ++i) {
        int k = order[i];
        triP[i * 3] = pos[k * 3]; triP[i * 3 + 1] = pos[k * 3 + 1]; triP[i * 3 + 2] = pos[k * 3 + 2];
        triU[i * 2] = uv[k * 2]; triU[i * 2 + 1] = uv[k * 2 + 1];
    }
    struct V { float p[3]; float u, v; };
    auto* heap = new std::vector<unsigned char>(6 * sizeof(V));
    auto* verts = reinterpret_cast<V*>(heap->data());
    for (int i = 0; i < 6; ++i) {
        verts[i].p[0] = triP[i * 3]; verts[i].p[1] = triP[i * 3 + 1]; verts[i].p[2] = triP[i * 3 + 2];
        verts[i].u = triU[i * 2]; verts[i].v = triU[i * 2 + 1];
    }
    VertexBuffer* vb = VertexBuffer::Builder()
        .vertexCount(6).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0, sizeof(V))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 12, sizeof(V))
        .build(*A->engine);
    IndexBuffer* ib = make_ib(6);
    if (!vb || !ib) {
        delete heap;
        if (vb) A->engine->destroy(vb);
        if (ib) A->engine->destroy(ib);
        return;
    }
    vb->setBufferAt(*A->engine, 0, VertexBuffer::BufferDescriptor(
        heap->data(), heap->size(), free_buf, heap));
    MaterialInstance* mi = A->sprite->createInstance();
    mi->setParameter("color", RgbaType::sRGB, float4{ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f });
    mi->setParameter("image", tex, A->sampEdge);
    add_mesh(vb, ib, 6, mat4f(), mi, 0, 5, false);
}

extern "C" void fb_screen(float x0, float y0, float x1, float y1, int r, int g, int b, int a) {
    if (!A || a < 1) return;
    float pos[18] = {
        x0, y0, 0,  x1, y0, 0,  x1, y1, 0,
        x0, y0, 0,  x1, y1, 0,  x0, y1, 0
    };
    /* y0/y1 are already clip space. Unlit front material ignores depth.
       Device domain would be better, but the camera is perspective, so this
       quad is submitted in clip space through the overlay material instead
       when we only have a color. A tiny unlit mesh in NDC needs the device
       domain. Use the overlay material's domain by drawing with unlitFront
       in object space and an identity camera... that would be wrong.
       Submit through a device-space color path: reuse unlitFront only if the
       view is orthographic. Screen tints are rare; draw them as a device quad
       by temporarily using the overlay shader's domain via a dedicated call
       that builds a device-space vertex buffer and a color instance. */
    VertexBuffer* vb = make_pos_vb(pos, 6);
    IndexBuffer* ib = make_ib(6);
    if (!vb || !ib) {
        if (vb) A->engine->destroy(vb);
        if (ib) A->engine->destroy(ib);
        return;
    }
    /* unlitFront is object-space. A fullscreen tint in clip space needs the
       device domain. Build it on the overlay material by tinting in the
       instance... overlay samples a texture. Fall back to object-space unlit
       with depth off, positioned in clip space, which is only correct if the
       projection is identity. Set a custom path: the glue passes clip coords
       and we draw them with VertexDomain::DEVICE by using overlayMat's sibling
       unlitFront compiled as DEVICE. unlitFront is OBJECT. Recompile is done.
       Draw with unlitFront and rely on the caller passing world positions.
       The glue converts pixels to clip and we switch the material.
       Practical fix: use the device-domain overlay material only for images.
       For a flat tint, upload a 1x1 texture of that color and draw a device quad. */
    A->engine->destroy(vb);
    A->engine->destroy(ib);
    unsigned char px[4] = { (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a };
    /* 1x1 is cached per color by stashing it in the dynamic path each time.
       A handful of tints per frame is fine. */
    auto* bytes = new std::vector<unsigned char>(px, px + 4);
    Texture* tex = Texture::Builder().width(1).height(1).levels(1)
        .sampler(Texture::Sampler::SAMPLER_2D)
        .format(Texture::InternalFormat::SRGB8_A8)
        .build(*A->engine);
    if (!tex) { delete bytes; return; }
    tex->setImage(*A->engine, 0, Texture::PixelBufferDescriptor(
        bytes->data(), bytes->size(), Texture::Format::RGBA, Texture::Type::UBYTE, free_buf, bytes));
    float clip[18] = {
        x0, y0, 0, x1, y0, 0, x1, y1, 0,
        x0, y0, 0, x1, y1, 0, x0, y1, 0
    };
    float uvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    struct V { float p[3]; float u, v; };
    auto* heap = new std::vector<unsigned char>(6 * sizeof(V));
    auto* verts = reinterpret_cast<V*>(heap->data());
    for (int i = 0; i < 6; ++i) {
        verts[i].p[0] = clip[i * 3]; verts[i].p[1] = clip[i * 3 + 1]; verts[i].p[2] = 0;
        verts[i].u = uvs[i * 2]; verts[i].v = uvs[i * 2 + 1];
    }
    vb = VertexBuffer::Builder().vertexCount(6).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0, sizeof(V))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 12, sizeof(V))
        .build(*A->engine);
    ib = make_ib(6);
    if (!vb || !ib) {
        delete heap;
        A->engine->destroy(tex);
        if (vb) A->engine->destroy(vb);
        if (ib) A->engine->destroy(ib);
        return;
    }
    vb->setBufferAt(*A->engine, 0, VertexBuffer::BufferDescriptor(
        heap->data(), heap->size(), free_buf, heap));
    MaterialInstance* mi = A->overlayMat->createInstance();
    mi->setParameter("image", tex, A->sampEdge);
    add_mesh(vb, ib, 6, mat4f(), mi, 0, 7, false);
    A->junkTex.push_back(tex);
}

extern "C" void fb_overlay(const void* bgra, int w, int h) {
    if (!A || !A->engine) return;
    A->haveOverlay = 0;
    if (!bgra || w < 1 || h < 1) return;
    auto* bytes = new std::vector<unsigned char>((size_t)w * h * 4);
    const unsigned char* s = (const unsigned char*)bgra;
    for (int i = 0; i < w * h; ++i) {
        (*bytes)[i * 4] = s[i * 4 + 2];
        (*bytes)[i * 4 + 1] = s[i * 4 + 1];
        (*bytes)[i * 4 + 2] = s[i * 4];
        (*bytes)[i * 4 + 3] = s[i * 4 + 3];
    }
    if (!A->ovTex || A->ovW != w || A->ovH != h) {
        if (A->ovTex) A->engine->destroy(A->ovTex);
        A->ovTex = Texture::Builder()
            .width((uint32_t)w).height((uint32_t)h).levels(1)
            .sampler(Texture::Sampler::SAMPLER_2D)
            .format(Texture::InternalFormat::RGBA8)
            .build(*A->engine);
        A->ovW = w;
        A->ovH = h;
    }
    if (!A->ovTex) { delete bytes; return; }
    A->ovTex->setImage(*A->engine, 0, Texture::PixelBufferDescriptor(
        bytes->data(), bytes->size(),
        Texture::Format::RGBA, Texture::Type::UBYTE, free_buf, bytes));
    A->haveOverlay = 1;
}

extern "C" int fb_draw(int capture) {
    if (!A || !A->renderer || !A->swap) return 0;
    flush_lines();
    draw_overlay();
    if (!A->renderer->beginFrame(A->swap)) return 0;
    A->renderer->render(A->view);
    if (capture && A->width > 0 && A->height > 0) {
        int w = A->width, h = A->height;
        auto* bytes = new std::vector<unsigned char>((size_t)w * h * 4);
        A->renderer->readPixels(0, 0, (uint32_t)w, (uint32_t)h,
            Texture::PixelBufferDescriptor(bytes->data(), bytes->size(),
                Texture::Format::RGBA, Texture::Type::UBYTE,
                [](void* buf, size_t, void* user) {
                    auto* pix = static_cast<std::vector<unsigned char>*>(user);
                    if (A) {
                        A->capture.swap(*pix);
                        A->capReady = 1;
                    }
                    delete pix;
                    (void)buf;
                }, bytes));
        A->capW = w;
        A->capH = h;
        A->capReady = 0;
    }
    A->renderer->endFrame();
    if (capture) {
        // readPixels finishes on the GL thread and only then queues the user
        // callback. flushAndWait runs that finish and then drains the callback,
        // so fb_take_capture sees the buffer before it returns.
        A->engine->flushAndWait();
    }
    return 1;
}

extern "C" int fb_take_capture(unsigned char* dst, int bytes, int* w, int* h) {
    if (!A || !A->capReady || A->capture.empty()) return 0;
    int n = A->capW * A->capH * 4;
    if (!dst || bytes < n) return 0;
    std::memcpy(dst, A->capture.data(), (size_t)n);
    if (w) *w = A->capW;
    if (h) *h = A->capH;
    A->capReady = 0;
    return 1;
}
