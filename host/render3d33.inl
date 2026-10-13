/* OpenGL 3.3 port of the immediate-mode draws in this file.
   The unit box, sphere, cylinder and model triangles are the same vertices.
   Lighting is the fixed-function equation the 1.1 path sets up in begin():
   scene ambient, up to eight diffuse lights, no specular, smooth shading,
   COLOR_MATERIAL ambient+diffuse, and two-sided lighting for models.
   The 1.1 path is untouched and stays the default. */

using CreateShaderFn = GLuint (APIENTRY *)(GLenum);
using ShaderSourceFn = void (APIENTRY *)(GLuint, GLsizei, const char* const*, const GLint*);
using CompileFn = void (APIENTRY *)(GLuint);
using GetShaderivFn = void (APIENTRY *)(GLuint, GLenum, GLint*);
using GetInfoFn = void (APIENTRY *)(GLuint, GLsizei, GLsizei*, char*);
using DeleteShaderFn = void (APIENTRY *)(GLuint);
using CreateProgFn = GLuint (APIENTRY *)();
using AttachFn2 = void (APIENTRY *)(GLuint, GLuint);
using LinkFn = void (APIENTRY *)(GLuint);
using GetProgivFn = void (APIENTRY *)(GLuint, GLenum, GLint*);
using UseFn = void (APIENTRY *)(GLuint);
using GetLocFn = GLint (APIENTRY *)(GLuint, const char*);
using UniMat4Fn = void (APIENTRY *)(GLint, GLsizei, GLboolean, const GLfloat*);
using UniMat3Fn = void (APIENTRY *)(GLint, GLsizei, GLboolean, const GLfloat*);
using Uni1iFn = void (APIENTRY *)(GLint, GLint);
using Uni1fFn = void (APIENTRY *)(GLint, GLfloat);
using Uni2fFn = void (APIENTRY *)(GLint, GLfloat, GLfloat);
using Uni3fvFn = void (APIENTRY *)(GLint, GLsizei, const GLfloat*);
using Uni4fvFn = void (APIENTRY *)(GLint, GLsizei, const GLfloat*);
using GenBufFn = void (APIENTRY *)(GLsizei, GLuint*);
using BindBufFn = void (APIENTRY *)(GLenum, GLuint);
using BufDataFn = void (APIENTRY *)(GLenum, std::intptr_t, const void*, GLenum);
using EnAttribFn = void (APIENTRY *)(GLuint);
using AttribPtrFn = void (APIENTRY *)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using GenVaoFn = void (APIENTRY *)(GLsizei, GLuint*);
using BindVaoFn = void (APIENTRY *)(GLuint);
using ActiveTexFn = void (APIENTRY *)(GLenum);

struct Proc33 {
    CreateShaderFn CreateShader = nullptr;
    ShaderSourceFn ShaderSource = nullptr;
    CompileFn Compile = nullptr;
    GetShaderivFn GetShaderiv = nullptr;
    GetInfoFn ShaderLog = nullptr;
    GetInfoFn ProgLog = nullptr;
    DeleteShaderFn DeleteShader = nullptr;
    CreateProgFn CreateProgram = nullptr;
    AttachFn2 Attach = nullptr;
    LinkFn Link = nullptr;
    GetProgivFn GetProgramiv = nullptr;
    UseFn Use = nullptr;
    GetLocFn GetLoc = nullptr;
    UniMat4Fn UniMat4 = nullptr;
    UniMat3Fn UniMat3 = nullptr;
    Uni1iFn Uni1i = nullptr;
    Uni1fFn Uni1f = nullptr;
    Uni2fFn Uni2f = nullptr;
    Uni3fvFn Uni3fv = nullptr;
    Uni4fvFn Uni4fv = nullptr;
    GenBufFn GenBuffers = nullptr;
    BindBufFn BindBuffer = nullptr;
    BufDataFn BufferData = nullptr;
    EnAttribFn EnableAttrib = nullptr;
    AttribPtrFn AttribPtr = nullptr;
    GenVaoFn GenVao = nullptr;
    BindVaoFn BindVao = nullptr;
    ActiveTexFn ActiveTex = nullptr;
};

struct Pipe {
    Proc33 P;
    GLuint prog = 0, vao = 0, vboBg = 0, vboLine = 0;
    GLuint vboBox = 0, vboSphere = 0, vboCyl = 0, vboCap = 0;
    GLsizei nBox = 0, nSphere = 0, nCyl = 0;
    std::unordered_map<const Model*, std::pair<GLuint, GLsizei>> models;
    float proj[16]{};
    float cam[16]{};
    float color[3]{1, 1, 1};
    float amb[3]{0.55f, 0.55f, 0.55f};
    float lpos[64]{};
    float ldif[48]{};
    float lspot[48]{};
    float lsp[48]{};
    int lights = 0;
    float lit = 1, two = 0, vert = 0, alpha = 1, ndcX = 0, ndcY = 0, fill = 0;
    float lightVP[16]{};
    float lightMvp[16]{};
    int shadowOn = 0;
    GLuint shProg = 0, shFbo = 0, shTex = 0;
    GLint uLightMvp = -1, uSun = -1, uDoShadow = -1, uShadowMap = -1, shUMvp = -1;
    GLint muLightMvp = -1, muSun = -1, muDoShadow = -1, muShadowMap = -1;
    GLint uFogColor = -1, uFogDist = -1, muFogColor = -1, muFogDist = -1;
    bool wideLines = true;
    GLint uMvp = -1, uMv = -1, uN = -1, uColor = -1, uAmb = -1;
    GLint uLit = -1, uTwo = -1, uVert = -1, uAlpha = -1, uNdc = -1, uLN = -1, uFill = -1;
    GLint uPos = -1, uDif = -1, uSpot = -1, uSp = -1;
    std::unordered_map<GLuint, GLuint> meshVao;
    GLuint matProg = 0;
    GLint muMvp = -1, muMv = -1, muN = -1, muColor = -1, muAmb = -1, muFill = -1;
    GLint muLit = -1, muAlpha = -1, muNdc = -1, muLN = -1, muMap = -1;
    GLint muPos = -1, muDif = -1, muSpot = -1, muSp = -1;
    GLint muBase = -1, muNrm = -1, muMetal = -1, muRough = -1, muHeight = -1;
    std::unordered_map<const Model*, GLuint> matTex;
    std::unordered_map<const Model*, GLuint> matVao;
    std::unordered_map<const Model*, std::pair<GLuint, GLsizei>> matMesh;
    GLuint skyProg = 0, skyVao = 0, skyVbo = 0, skyTex[6]{};
    GLint skyUView = -1, skyUFog = -1, skyUFogOn = -1, skyUFace[6]{};
    std::string skyStamp;
    GLuint partProg = 0, partVao = 0, partVbo = 0;
    GLint partUView = -1, partUProj = -1, partUTex = -1, partUFog = -1, partUFogDist = -1;
};

static Pipe K;
static std::vector<float> g_lines;
static std::vector<float> g_scratch;
static int g_lineA = -1;
static float g_sentLit = -1, g_sentTwo = -1, g_sentVert = -1, g_sentAlpha = -1;
static float g_sentNdcX = 999, g_sentNdcY = 999;

constexpr GLenum kArray = 0x8892, kStatic = 0x88E4, kDynamic = 0x88E8;

static void ident(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void mul(float* o, const float* a, const float* b) {
    float t[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            t[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1]
                         + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        }
    }
    std::memcpy(o, t, sizeof(t));
}

static void xform(const float* m, float x, float y, float z, float w, float* o) {
    o[0] = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
    o[1] = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
    o[2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
    o[3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
}

/* Inverse-transpose of the modelview's upper 3x3: what GL_NORMALIZE lights with. */
static void normal_matrix(const float* m, float* n) {
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float g = m[2], h = m[6], i = m[10];
    float C00 = e * i - f * h, C10 = f * g - d * i, C20 = d * h - e * g;
    float C01 = c * h - b * i, C11 = a * i - c * g, C21 = b * g - a * h;
    float C02 = b * f - c * e, C12 = c * d - a * f, C22 = a * e - b * d;
    float det = a * C00 + b * C10 + c * C20;
    float inv = std::fabs(det) < 1e-12f ? 0.0f : 1.0f / det;
    n[0] = C00 * inv; n[1] = C10 * inv; n[2] = C20 * inv;
    n[3] = C01 * inv; n[4] = C11 * inv; n[5] = C21 * inv;
    n[6] = C02 * inv; n[7] = C12 * inv; n[8] = C22 * inv;
}

static void push9(std::vector<float>& o, float x, float y, float z, float nx, float ny, float nz,
                  float r, float g, float b) {
    float v[9] = { x, y, z, nx, ny, nz, r, g, b };
    o.insert(o.end(), v, v + 9);
}

/* GL_QUADS (v0,v1,v2,v3) as the two triangles that cover the same quad. */
static void push_quad(std::vector<float>& o, const float* v0, const float* v1,
                      const float* v2, const float* v3) {
    o.insert(o.end(), v0, v0 + 9);
    o.insert(o.end(), v1, v1 + 9);
    o.insert(o.end(), v2, v2 + 9);
    o.insert(o.end(), v0, v0 + 9);
    o.insert(o.end(), v2, v2 + 9);
    o.insert(o.end(), v3, v3 + 9);
}

/* GL_QUAD_STRIP. The quad between pair i and i+1 is (v0, v1, v3, v2). */
static void push_strip(std::vector<float>& o, const std::vector<float>& s) {
    int pairs = (int)s.size() / 9 / 2;
    for (int i = 0; i + 1 < pairs; ++i) {
        const float* v0 = &s[(2 * i) * 9];
        const float* v1 = &s[(2 * i + 1) * 9];
        const float* v2 = &s[(2 * i + 2) * 9];
        const float* v3 = &s[(2 * i + 3) * 9];
        o.insert(o.end(), v0, v0 + 9);
        o.insert(o.end(), v1, v1 + 9);
        o.insert(o.end(), v3, v3 + 9);
        o.insert(o.end(), v0, v0 + 9);
        o.insert(o.end(), v3, v3 + 9);
        o.insert(o.end(), v2, v2 + 9);
    }
}

static void push_fan(std::vector<float>& o, const std::vector<float>& f) {
    int n = (int)f.size() / 9;
    for (int i = 1; i + 1 < n; ++i) {
        o.insert(o.end(), f.begin(), f.begin() + 9);
        o.insert(o.end(), f.begin() + i * 9, f.begin() + i * 9 + 9);
        o.insert(o.end(), f.begin() + (i + 1) * 9, f.begin() + (i + 1) * 9 + 9);
    }
}

static GLuint upload(const std::vector<float>& v, GLsizei* n) {
    GLuint id = 0;
    K.P.GenBuffers(1, &id);
    K.P.BindBuffer(kArray, id);
    K.P.BufferData(kArray, (std::intptr_t)(v.size() * sizeof(float)), v.data(), kStatic);
    *n = (GLsizei)(v.size() / 9);
    return id;
}

/* The same loops as build_lists(), triangles where a core context has no quads. */
static bool build_unit_meshes() {
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
    const int GRID = 24;
    for (int f = 0; f < 6; ++f) {
        auto corner = [&](float u, float v, float* p) {
            for (int c = 0; c < 3; ++c) {
                float a = F[f][0][c] + (F[f][1][c] - F[f][0][c]) * u;
                float b = F[f][3][c] + (F[f][2][c] - F[f][3][c]) * u;
                p[c] = a + (b - a) * v;
            }
            p[3] = N[f][0]; p[4] = N[f][1]; p[5] = N[f][2];
            p[6] = p[7] = p[8] = 1;
        };
        for (int i = 0; i < GRID; ++i) {
            for (int j = 0; j < GRID; ++j) {
                float u0 = (float)i / GRID, u1 = (float)(i + 1) / GRID;
                float v0 = (float)j / GRID, v1 = (float)(j + 1) / GRID;
                float q[4][9];
                corner(u0, v0, q[0]); corner(u1, v0, q[1]);
                corner(u1, v1, q[2]); corner(u0, v1, q[3]);
                push_quad(box, q[0], q[1], q[2], q[3]);
            }
        }
    }
    const int SEG = 40, RING = 24;
    for (int j = 0; j < RING; ++j) {
        std::vector<float> strip;
        for (int i = 0; i <= SEG; ++i) {
            for (int k = 0; k < 2; ++k) {
                float th = (float)(j + k) / RING * 3.14159265f;
                float ph = (float)i / SEG * 6.2831853f;
                float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
                push9(strip, x * .5f, y * .5f, z * .5f, x, y, z, 1, 1, 1);
            }
        }
        push_strip(sphere, strip);
    }
    const int CS = 48;
    std::vector<float> side;
    for (int i = 0; i <= CS; ++i) {
        float a = (float)i / CS * 6.2831853f;
        float x = std::cos(a), z = std::sin(a);
        push9(side, x * .5f, .5f, z * .5f, x, 0, z, 1, 1, 1);
        push9(side, x * .5f, -.5f, z * .5f, x, 0, z, 1, 1, 1);
    }
    push_strip(cyl, side);
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap == 0 ? .5f : -.5f;
        float ny = cap == 0 ? 1.0f : -1.0f;
        std::vector<float> fan;
        push9(fan, 0, y, 0, 0, ny, 0, 1, 1, 1);
        for (int i = 0; i <= CS; ++i) {
            float a = (float)i / CS * 6.2831853f;
            push9(fan, std::cos(a) * .5f, y, std::sin(a) * .5f, 0, ny, 0, 1, 1, 1);
        }
        push_fan(cyl, fan);
    }
    K.vboBox = upload(box, &K.nBox);
    K.vboSphere = upload(sphere, &K.nSphere);
    K.vboCyl = upload(cyl, &K.nCyl);
    K.P.GenBuffers(1, &K.vboBg);
    K.P.GenBuffers(1, &K.vboLine);
    static const float cube[] = {
        1,-1,-1,  1, 1,-1,  1, 1, 1,   1,-1,-1,  1, 1, 1,  1,-1, 1,
       -1,-1, 1, -1, 1, 1, -1, 1,-1,  -1,-1, 1, -1, 1,-1, -1,-1,-1,
       -1, 1,-1, -1, 1, 1,  1, 1, 1,  -1, 1,-1,  1, 1, 1,  1, 1,-1,
       -1,-1, 1, -1,-1,-1,  1,-1,-1,  -1,-1, 1,  1,-1,-1,  1,-1, 1,
       -1,-1, 1,  1,-1, 1,  1, 1, 1,  -1,-1, 1,  1, 1, 1, -1, 1, 1,
        1,-1,-1, -1,-1,-1, -1, 1,-1,   1,-1,-1, -1, 1,-1,  1, 1,-1,
    };
    K.P.GenBuffers(1, &K.skyVbo);
    K.P.BindBuffer(kArray, K.skyVbo);
    K.P.BufferData(kArray, (std::intptr_t)sizeof(cube), cube, kStatic);
    K.P.GenVao(1, &K.skyVao);
    K.P.BindVao(K.skyVao);
    K.P.EnableAttrib(0);
    K.P.AttribPtr(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
    K.P.BindVao(0);
    return K.vboBox && K.vboSphere && K.vboCyl && K.nBox > 0 && K.skyVbo && K.skyVao;
}

static GLuint model_vbo(const Model& m, GLsizei* n) {
    auto it = K.models.find(&m);
    if (it != K.models.end()) { *n = it->second.second; return it->second.first; }
    std::vector<float> v;
    for (size_t k = 0; k + 2 < m.tri.size(); k += 3) {
        V3 a = m.pos[m.tri[k]], b = m.pos[m.tri[k + 1]], c = m.pos[m.tri[k + 2]];
        V3 nm = norm(cross(b - a, c - a));
        push9(v, a.x, a.y, a.z, nm.x, nm.y, nm.z, 1, 1, 1);
        push9(v, b.x, b.y, b.z, nm.x, nm.y, nm.z, 1, 1, 1);
        push9(v, c.x, c.y, c.z, nm.x, nm.y, nm.z, 1, 1, 1);
    }
    GLsizei count = 0;
    GLuint id = v.empty() ? 0 : upload(v, &count);
    K.models[&m] = { id, count };
    *n = count;
    return id;
}

static const char* kVs = R"GLSL(
#version 330
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec3 aCol;
uniform mat4 uMvp;
uniform mat4 uMv;
uniform mat3 uNmat;
uniform vec3 uColor;
uniform vec3 uAmb;
uniform float uFill;
uniform float uLit;
uniform float uTwo;
uniform float uVert;
uniform vec2 uNdc;
uniform int uLN;
uniform vec4 uLPos[16];
uniform vec3 uLDif[16];
uniform vec3 uLSpot[16];
uniform vec3 uLSp[16];
uniform int uSun;
uniform mat4 uLightMvp;
uniform float uFogDist;
out vec3 vFront;
out vec3 vBack;
out vec3 vSunF;
out vec3 vSunB;
out vec4 vLight;
out float vFog;
vec3 shade(vec3 n, vec3 pe, vec3 mat, int skip) {
    vec3 direct = vec3(0.0);
    for (int i = 0; i < 16; i++) {
        if (i >= uLN) break;
        if (i == skip) continue;
        vec3 toL;
        float att = 1.0;
        if (uLPos[i].w == 0.0) {
            toL = normalize(uLPos[i].xyz);
        } else {
            vec3 d = uLPos[i].xyz - pe;
            float dist = length(d);
            if (dist < 1e-5) toL = vec3(0.0, 0.0, 1.0);
            else { toL = d / dist; att = 1.0 / (1.0 + uLSp[i].z * dist * dist); }
        }
        float spot = 1.0;
        if (uLSp[i].x < 1.5) {
            float s = dot(-toL, normalize(uLSpot[i]));
            if (s < uLSp[i].x) spot = 0.0;
            else spot = pow(max(s, 0.0), uLSp[i].y);
        }
        direct += uLDif[i] * max(dot(n, toL), 0.0) * att * spot;
    }
    vec3 lit = uAmb + direct;
    /* An ambient light fills whatever the sun left dark. Added on top, it
       was clipped away on any face the sun had already lit, so intensity
       looked like it did nothing. */
    if (uFill > 0.5) {
        vec3 d = clamp(direct, 0.0, 1.0);
        lit = d + uAmb * (vec3(1.0) - d);
    }
    return clamp(lit * mat, 0.0, 1.0);
}
void main() {
    vec4 eye = uMv * vec4(aPos, 1.0);
    gl_Position = uMvp * vec4(aPos, 1.0);
    gl_Position.xy += uNdc * gl_Position.w;
    vLight = uLightMvp * vec4(aPos, 1.0);
    vFog = 0.0;
    if (uFogDist > 0.001) {
        float dist = length(eye.xyz);
        float start = uFogDist * 0.25;
        vFog = clamp((dist - start) / max(uFogDist - start, 0.001), 0.0, 1.0);
    }
    if (uLit < 0.5) {
        vec3 col = (uVert > 0.5) ? aCol : uColor;
        vFront = col;
        vBack = col;
        vSunF = vec3(0.0);
        vSunB = vec3(0.0);
    } else {
        vec3 n = normalize(uNmat * aNrm);
        vFront = shade(n, eye.xyz, uColor, -1);
        vSunF = vFront - shade(n, eye.xyz, uColor, uSun);
        if (uTwo > 0.5) {
            vBack = shade(-n, eye.xyz, uColor, -1);
            vSunB = vBack - shade(-n, eye.xyz, uColor, uSun);
        } else {
            vBack = vFront;
            vSunB = vSunF;
        }
    }
}
)GLSL";

static const char* kSkyVs = R"GLSL(
#version 330
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
out vec3 vDir;
void main() {
    vec4 clip = uView * vec4(aPos, 1.0);
    gl_Position = clip.xyww;
    vDir = aPos;
}
)GLSL";

static const char* kSkyFs = R"GLSL(
#version 330
in vec3 vDir;
uniform sampler2D uFace[6];
uniform vec3 uFogColor;
uniform float uFogOn;
out vec4 frag;
void main() {
    vec3 d = normalize(vDir);
    vec3 a = abs(d);
    vec2 uv;
    vec3 c;
    if (a.x >= a.y && a.x >= a.z) {
        if (d.x > 0.0) { uv = vec2(-d.z, d.y) / a.x; c = texture(uFace[0], uv * 0.5 + 0.5).rgb; }
        else           { uv = vec2( d.z, d.y) / a.x; c = texture(uFace[1], uv * 0.5 + 0.5).rgb; }
    } else if (a.y >= a.z) {
        if (d.y > 0.0) { uv = vec2( d.x,-d.z) / a.y; c = texture(uFace[2], uv * 0.5 + 0.5).rgb; }
        else           { uv = vec2( d.x, d.z) / a.y; c = texture(uFace[3], uv * 0.5 + 0.5).rgb; }
    } else {
        if (d.z > 0.0) { uv = vec2( d.x, d.y) / a.z; c = texture(uFace[4], uv * 0.5 + 0.5).rgb; }
        else           { uv = vec2(-d.x, d.y) / a.z; c = texture(uFace[5], uv * 0.5 + 0.5).rgb; }
    }
    float horizon = pow(clamp(1.0 - abs(d.y), 0.0, 1.0), 1.4);
    c = mix(c, uFogColor, horizon * uFogOn);
    frag = vec4(c, 1.0);
}
)GLSL";

static const char* kFs = R"GLSL(
#version 330
in vec3 vFront;
in vec3 vBack;
in vec3 vSunF;
in vec3 vSunB;
in vec4 vLight;
in float vFog;
uniform float uAlpha;
uniform float uDoShadow;
uniform sampler2D uShadowMap;
uniform vec3 uFogColor;
out vec4 frag;
float shadow_pcf(vec4 clip) {
    if (clip.w <= 0.0) return 1.0;
    vec3 p = clip.xyz / clip.w * 0.5 + 0.5;
    if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;
    const vec2 kOff[16] = vec2[](
        vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
        vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
        vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
        vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
        vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19048895),
        vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
        vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
    );
    float s = 0.0;
    float scale = 48.0 / 4096.0;
    for (int i = 0; i < 16; i++) {
        float d = texture(uShadowMap, p.xy + kOff[i] * scale).r;
        s += (p.z - 0.0012 > d) ? 0.0 : 1.0;
    }
    return s / 16.0;
}
void main() {
    vec3 col = gl_FrontFacing ? vFront : vBack;
    vec3 sun = gl_FrontFacing ? vSunF : vSunB;
    if (uDoShadow > 0.5) col -= sun * (1.0 - shadow_pcf(vLight));
    col = mix(clamp(col, 0.0, 1.0), uFogColor, vFog);
    frag = vec4(col, uAlpha);
}
)GLSL";

static const char* kMatVs = R"GLSL(
#version 330
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec4 aTan;
layout(location = 3) in vec2 aUv;
uniform mat4 uMvp;
uniform mat4 uMv;
uniform mat3 uNmat;
uniform vec2 uNdc;
uniform mat4 uLightMvp;
uniform float uFogDist;
out vec3 vEye;
out vec4 vLight;
out float vFog;
out vec3 vN;
out vec3 vT;
out vec3 vB;
out vec2 vUv;
void main() {
    vec4 eye = uMv * vec4(aPos, 1.0);
    gl_Position = uMvp * vec4(aPos, 1.0);
    gl_Position.xy += uNdc * gl_Position.w;
    vEye = eye.xyz;
    vFog = 0.0;
    if (uFogDist > 0.001) {
        float dist = length(eye.xyz);
        float start = uFogDist * 0.25;
        vFog = clamp((dist - start) / max(uFogDist - start, 0.001), 0.0, 1.0);
    }
    vN = normalize(uNmat * aNrm);
    vT = normalize(uNmat * aTan.xyz);
    vB = cross(vN, vT) * aTan.w;
    vUv = aUv;
    vLight = uLightMvp * vec4(aPos, 1.0);
}
)GLSL";

static const char* kMatFs = R"GLSL(
#version 330
in vec3 vEye;
in vec3 vN;
in vec3 vT;
in vec3 vB;
in vec2 vUv;
in vec4 vLight;
in float vFog;
uniform vec3 uColor;
uniform vec3 uFogColor;
uniform vec3 uAmb;
uniform float uFill;
uniform float uLit;
uniform float uAlpha;
uniform int uLN;
uniform int uMap;
uniform vec4 uLPos[16];
uniform vec3 uLDif[16];
uniform vec3 uLSpot[16];
uniform vec3 uLSp[16];
uniform sampler2D uBase;
uniform sampler2D uNrm;
uniform sampler2D uMetal;
uniform sampler2D uRough;
uniform sampler2D uHeight;
uniform int uSun;
uniform float uDoShadow;
uniform sampler2D uShadowMap;
out vec4 frag;
float shadow_pcf(vec4 clip) {
    if (clip.w <= 0.0) return 1.0;
    vec3 p = clip.xyz / clip.w * 0.5 + 0.5;
    if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;
    const vec2 kOff[16] = vec2[](
        vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
        vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
        vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
        vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
        vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19048895),
        vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
        vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
    );
    float s = 0.0;
    float scale = 48.0 / 4096.0;
    for (int i = 0; i < 16; i++) {
        float d = texture(uShadowMap, p.xy + kOff[i] * scale).r;
        s += (p.z - 0.0012 > d) ? 0.0 : 1.0;
    }
    return s / 16.0;
}
vec3 shade(vec3 n, vec3 pe, vec3 albedo, float metal, float rough) {
    vec3 direct = vec3(0.0);
    vec3 spec = vec3(0.0);
    vec3 V = normalize(-pe);
    float shin = mix(96.0, 6.0, clamp(rough, 0.0, 1.0));
    vec3 F0 = mix(vec3(0.04), albedo, metal);
    for (int i = 0; i < 16; i++) {
        if (i >= uLN) break;
        vec3 toL;
        float att = 1.0;
        if (uLPos[i].w == 0.0) {
            toL = normalize(uLPos[i].xyz);
        } else {
            vec3 d = uLPos[i].xyz - pe;
            float dist = length(d);
            if (dist < 1e-5) toL = vec3(0.0, 0.0, 1.0);
            else { toL = d / dist; att = 1.0 / (1.0 + uLSp[i].z * dist * dist); }
        }
        float spot = 1.0;
        if (uLSp[i].x < 1.5) {
            float s = dot(-toL, normalize(uLSpot[i]));
            if (s < uLSp[i].x) spot = 0.0;
            else spot = pow(max(s, 0.0), uLSp[i].y);
        }
        float ndl = max(dot(n, toL), 0.0);
        float sh = 1.0;
        if (uDoShadow > 0.5 && i == uSun) sh = shadow_pcf(vLight);
        float lamp = ndl * att * spot * sh;
        direct += uLDif[i] * lamp;
        vec3 H = normalize(toL + V);
        spec += uLDif[i] * pow(max(dot(n, H), 0.0), shin) * att * spot * sh;
    }
    vec3 lit = uAmb + direct;
    if (uFill > 0.5) {
        vec3 d = clamp(direct, 0.0, 1.0);
        lit = d + uAmb * (vec3(1.0) - d);
    }
    vec3 diff = albedo * (1.0 - metal);
    return clamp(lit * diff + spec * F0 * 0.18, 0.0, 1.0);
}
void main() {
    vec2 uv = vUv;
    vec3 nrm = normalize(gl_FrontFacing ? vN : -vN);
    vec3 tan = normalize(gl_FrontFacing ? vT : -vT);
    vec3 bit = normalize(gl_FrontFacing ? vB : -vB);
    vec3 albedo = ((uMap & 1) != 0) ? texture(uBase, uv).rgb : uColor;
    if ((uMap & 2) != 0) {
        vec3 tn = texture(uNrm, uv).rgb * 2.0 - 1.0;
        nrm = normalize(tan * tn.x + bit * tn.y + nrm * tn.z);
    }
    /* Height stays on this chart. Sliding the coordinate would pull the
       doorway onto the floor picture beside it in the texture. */
    if ((uMap & 16) != 0) {
        float h = texture(uHeight, uv).r;
        vec2 dx = dFdx(uv);
        vec2 dy = dFdy(uv);
        float hx = texture(uHeight, uv + dx).r - h;
        float hy = texture(uHeight, uv + dy).r - h;
        nrm = normalize(nrm - tan * hx * 4.0 - bit * hy * 4.0);
    }
    float metal = ((uMap & 4) != 0) ? texture(uMetal, uv).r : 0.0;
    float rough = ((uMap & 8) != 0) ? texture(uRough, uv).r : 0.55;
    vec3 col = (uLit < 0.5) ? albedo : shade(nrm, vEye, albedo, metal, rough);
    col = mix(col, uFogColor, vFog);
    frag = vec4(col, uAlpha);
}
)GLSL";

static GLuint compile_one(GLenum kind, const char* src) {
    GLuint s = K.P.CreateShader(kind);
    if (!s) return 0;
    K.P.ShaderSource(s, 1, &src, nullptr);
    K.P.Compile(s);
    GLint ok = 0;
    K.P.GetShaderiv(s, 0x8B81, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        if (K.P.ShaderLog) K.P.ShaderLog(s, 1023, &n, log);
        log[n > 0 ? n : 0] = 0;
        std::fprintf(stderr, "OpenGL 3.3 shader: %s\n", log);
        K.P.DeleteShader(s);
        return 0;
    }
    return s;
}

static bool build_program() {
    GLuint vs = compile_one(0x8B31, kVs);
    GLuint fs = compile_one(0x8B30, kFs);
    if (!vs || !fs) return false;
    K.prog = K.P.CreateProgram();
    if (!K.prog) return false;
    K.P.Attach(K.prog, vs);
    K.P.Attach(K.prog, fs);
    K.P.Link(K.prog);
    GLint ok = 0;
    K.P.GetProgramiv(K.prog, 0x8B82, &ok);
    K.P.DeleteShader(vs);
    K.P.DeleteShader(fs);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        if (K.P.ProgLog) K.P.ProgLog(K.prog, 1023, &n, log);
        log[n > 0 ? n : 0] = 0;
        std::fprintf(stderr, "OpenGL 3.3 link: %s\n", log);
        return false;
    }
    K.P.Use(K.prog);
    K.uMvp = K.P.GetLoc(K.prog, "uMvp");
    K.uMv = K.P.GetLoc(K.prog, "uMv");
    K.uN = K.P.GetLoc(K.prog, "uNmat");
    K.uColor = K.P.GetLoc(K.prog, "uColor");
    K.uAmb = K.P.GetLoc(K.prog, "uAmb");
    K.uFill = K.P.GetLoc(K.prog, "uFill");
    K.uLit = K.P.GetLoc(K.prog, "uLit");
    K.uTwo = K.P.GetLoc(K.prog, "uTwo");
    K.uVert = K.P.GetLoc(K.prog, "uVert");
    K.uAlpha = K.P.GetLoc(K.prog, "uAlpha");
    K.uNdc = K.P.GetLoc(K.prog, "uNdc");
    K.uLN = K.P.GetLoc(K.prog, "uLN");
    K.uPos = K.P.GetLoc(K.prog, "uLPos[0]");
    K.uDif = K.P.GetLoc(K.prog, "uLDif[0]");
    K.uSpot = K.P.GetLoc(K.prog, "uLSpot[0]");
    K.uSp = K.P.GetLoc(K.prog, "uLSp[0]");
    K.uLightMvp = K.P.GetLoc(K.prog, "uLightMvp");
    K.uSun = K.P.GetLoc(K.prog, "uSun");
    K.uDoShadow = K.P.GetLoc(K.prog, "uDoShadow");
    K.uShadowMap = K.P.GetLoc(K.prog, "uShadowMap");
    if (K.uShadowMap >= 0) K.P.Uni1i(K.uShadowMap, 7);
    if (K.uDoShadow >= 0) K.P.Uni1f(K.uDoShadow, 0);
    if (K.uSun >= 0) K.P.Uni1i(K.uSun, -1);
    K.uFogColor = K.P.GetLoc(K.prog, "uFogColor");
    K.uFogDist = K.P.GetLoc(K.prog, "uFogDist");
    if (K.uFogDist >= 0) K.P.Uni1f(K.uFogDist, 0);
    if (!(K.uMvp >= 0 && K.uMv >= 0 && K.uN >= 0)) return false;
    static const char* kShVs = R"GLSL(
#version 330
layout(location = 0) in vec3 aPos;
uniform mat4 uMvp;
void main() { gl_Position = uMvp * vec4(aPos, 1.0); }
)GLSL";
    static const char* kShFs = R"GLSL(
#version 330
void main() {}
)GLSL";
    GLuint shVs = compile_one(0x8B31, kShVs);
    GLuint shFs = compile_one(0x8B30, kShFs);
    if (shVs && shFs) {
        K.shProg = K.P.CreateProgram();
        if (K.shProg) {
            K.P.Attach(K.shProg, shVs);
            K.P.Attach(K.shProg, shFs);
            K.P.Link(K.shProg);
            GLint shOk = 0;
            K.P.GetProgramiv(K.shProg, 0x8B82, &shOk);
            if (!shOk) K.shProg = 0;
            else K.shUMvp = K.P.GetLoc(K.shProg, "uMvp");
        }
    }
    if (shVs) K.P.DeleteShader(shVs);
    if (shFs) K.P.DeleteShader(shFs);
    GLuint mvs = compile_one(0x8B31, kMatVs);
    GLuint mfs = compile_one(0x8B30, kMatFs);
    if (mvs && mfs) {
        K.matProg = K.P.CreateProgram();
        if (K.matProg) {
            K.P.Attach(K.matProg, mvs);
            K.P.Attach(K.matProg, mfs);
            K.P.Link(K.matProg);
            GLint mok = 0;
            K.P.GetProgramiv(K.matProg, 0x8B82, &mok);
            if (!mok) {
                char log[1024];
                GLsizei n = 0;
                if (K.P.ProgLog) K.P.ProgLog(K.matProg, 1023, &n, log);
                log[n > 0 ? n : 0] = 0;
                std::fprintf(stderr, "OpenGL 3.3 material: %s\n", log);
                K.matProg = 0;
            } else {
                K.P.Use(K.matProg);
                K.muMvp = K.P.GetLoc(K.matProg, "uMvp");
                K.muMv = K.P.GetLoc(K.matProg, "uMv");
                K.muN = K.P.GetLoc(K.matProg, "uNmat");
                K.muColor = K.P.GetLoc(K.matProg, "uColor");
                K.muAmb = K.P.GetLoc(K.matProg, "uAmb");
                K.muFill = K.P.GetLoc(K.matProg, "uFill");
                K.muLit = K.P.GetLoc(K.matProg, "uLit");
                K.muAlpha = K.P.GetLoc(K.matProg, "uAlpha");
                K.muNdc = K.P.GetLoc(K.matProg, "uNdc");
                K.muLN = K.P.GetLoc(K.matProg, "uLN");
                K.muMap = K.P.GetLoc(K.matProg, "uMap");
                K.muPos = K.P.GetLoc(K.matProg, "uLPos[0]");
                K.muDif = K.P.GetLoc(K.matProg, "uLDif[0]");
                K.muSpot = K.P.GetLoc(K.matProg, "uLSpot[0]");
                K.muSp = K.P.GetLoc(K.matProg, "uLSp[0]");
                K.muBase = K.P.GetLoc(K.matProg, "uBase");
                K.muNrm = K.P.GetLoc(K.matProg, "uNrm");
                K.muMetal = K.P.GetLoc(K.matProg, "uMetal");
                K.muRough = K.P.GetLoc(K.matProg, "uRough");
                K.muHeight = K.P.GetLoc(K.matProg, "uHeight");
                if (K.muBase >= 0) K.P.Uni1i(K.muBase, 0);
                if (K.muNrm >= 0) K.P.Uni1i(K.muNrm, 1);
                if (K.muMetal >= 0) K.P.Uni1i(K.muMetal, 2);
                if (K.muRough >= 0) K.P.Uni1i(K.muRough, 3);
                if (K.muHeight >= 0) K.P.Uni1i(K.muHeight, 4);
                K.muLightMvp = K.P.GetLoc(K.matProg, "uLightMvp");
                K.muSun = K.P.GetLoc(K.matProg, "uSun");
                K.muDoShadow = K.P.GetLoc(K.matProg, "uDoShadow");
                K.muShadowMap = K.P.GetLoc(K.matProg, "uShadowMap");
                if (K.muShadowMap >= 0) K.P.Uni1i(K.muShadowMap, 7);
                if (K.muDoShadow >= 0) K.P.Uni1f(K.muDoShadow, 0);
                if (K.muSun >= 0) K.P.Uni1i(K.muSun, -1);
                K.muFogColor = K.P.GetLoc(K.matProg, "uFogColor");
                K.muFogDist = K.P.GetLoc(K.matProg, "uFogDist");
                if (K.muFogDist >= 0) K.P.Uni1f(K.muFogDist, 0);
            }
        }
    }
    if (mvs) K.P.DeleteShader(mvs);
    if (mfs) K.P.DeleteShader(mfs);
    GLuint svs = compile_one(0x8B31, kSkyVs);
    GLuint sfs = compile_one(0x8B30, kSkyFs);
    if (!svs || !sfs) return true;
    K.skyProg = K.P.CreateProgram();
    if (!K.skyProg) return true;
    K.P.Attach(K.skyProg, svs);
    K.P.Attach(K.skyProg, sfs);
    K.P.Link(K.skyProg);
    GLint skyOk = 0;
    K.P.GetProgramiv(K.skyProg, 0x8B82, &skyOk);
    K.P.DeleteShader(svs);
    K.P.DeleteShader(sfs);
    if (!skyOk) {
        char log[1024];
        GLsizei n = 0;
        if (K.P.ProgLog) K.P.ProgLog(K.skyProg, 1023, &n, log);
        log[n > 0 ? n : 0] = 0;
        std::fprintf(stderr, "OpenGL 3.3 sky: %s\n", log);
        K.skyProg = 0;
        return true;
    }
    K.skyUView = K.P.GetLoc(K.skyProg, "uView");
    K.skyUFog = K.P.GetLoc(K.skyProg, "uFogColor");
    K.skyUFogOn = K.P.GetLoc(K.skyProg, "uFogOn");
    for (int i = 0; i < 6; ++i) {
        std::string name = "uFace[" + std::to_string(i) + "]";
        K.skyUFace[i] = K.P.GetLoc(K.skyProg, name.c_str());
    }
    return true;
}

static bool load_procs() {
#define LOAD(field, name) K.P.field = (decltype(K.P.field))proc(name); if (!K.P.field) return false;
    LOAD(CreateShader, "glCreateShader")
    LOAD(ShaderSource, "glShaderSource")
    LOAD(Compile, "glCompileShader")
    LOAD(GetShaderiv, "glGetShaderiv")
    LOAD(DeleteShader, "glDeleteShader")
    LOAD(CreateProgram, "glCreateProgram")
    LOAD(Attach, "glAttachShader")
    LOAD(Link, "glLinkProgram")
    LOAD(GetProgramiv, "glGetProgramiv")
    LOAD(Use, "glUseProgram")
    LOAD(GetLoc, "glGetUniformLocation")
    LOAD(UniMat4, "glUniformMatrix4fv")
    LOAD(UniMat3, "glUniformMatrix3fv")
    LOAD(Uni1i, "glUniform1i")
    LOAD(Uni1f, "glUniform1f")
    LOAD(Uni2f, "glUniform2f")
    LOAD(Uni3fv, "glUniform3fv")
    LOAD(Uni4fv, "glUniform4fv")
    LOAD(GenBuffers, "glGenBuffers")
    LOAD(BindBuffer, "glBindBuffer")
    LOAD(BufferData, "glBufferData")
    LOAD(EnableAttrib, "glEnableVertexAttribArray")
    LOAD(AttribPtr, "glVertexAttribPointer")
    LOAD(GenVao, "glGenVertexArrays")
    LOAD(BindVao, "glBindVertexArray")
#undef LOAD
    K.P.ShaderLog = (GetInfoFn)proc("glGetShaderInfoLog");
    K.P.ProgLog = (GetInfoFn)proc("glGetProgramInfoLog");
    K.P.ActiveTex = (ActiveTexFn)proc("glActiveTexture");
    return true;
}

static void kill_rc() {
    wglMakeCurrent(nullptr, nullptr);
    if (S.rc) wglDeleteContext(S.rc);
    S.rc = nullptr;
    K = Pipe{};
}

static bool try_core() {
    HGLRC temp = wglCreateContext(S.dc);
    if (!temp || !wglMakeCurrent(S.dc, temp)) {
        if (temp) wglDeleteContext(temp);
        return false;
    }
    using CreateAttribs = HGLRC (APIENTRY *)(HDC, HGLRC, const int*);
    auto create = (CreateAttribs)proc("wglCreateContextAttribsARB");
    if (!create) { kill_rc(); S.rc = temp; kill_rc(); return false; }
    /* Compatibility keeps glLineWidth, which is how the 1.1 path draws the
       selection outline. Core is the fallback where a driver has no compatibility. */
    const int compat[] = { 0x2091, 3, 0x2092, 3, 0x9126, 0x00000002, 0 };
    const int coreBits[] = { 0x2091, 3, 0x2092, 3, 0x9126, 0x00000001, 0 };
    HGLRC core = create(S.dc, nullptr, compat);
    if (!core) core = create(S.dc, nullptr, coreBits);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(temp);
    if (!core || !wglMakeCurrent(S.dc, core)) {
        if (core) wglDeleteContext(core);
        return false;
    }
    S.rc = core;
    if (!load_procs() || !build_program() || !load_fbo() || !build_unit_meshes()) {
        kill_rc();
        return false;
    }
    K.P.GenVao(1, &K.vao);
    if (!K.vao) { kill_rc(); return false; }
    K.P.BindVao(K.vao);
    float range[2] = { 1, 1 };
    glGetFloatv(0x846E, range);
    K.wideLines = range[1] >= 4.0f;
    /* Compatibility accepts GL_BGRA, so the readback can land in the DIB.
       A core fallback does not, and finish() swizzles that case. */
    GLint profile = 0;
    glGetIntegerv(0x9126, &profile);
    S.bgra = (profile & 0x00000002) != 0;
    return true;
}

static void drop() {
    if (S.vk) nexa_vk14::shutdown();
    if (S.rc) {
        wglMakeCurrent(S.dc, S.rc);
        part_gl_forget();
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(S.rc);
    }
    if (S.dc && S.wnd) ReleaseDC(S.wnd, S.dc);
    if (S.wnd) DestroyWindow(S.wnd);
    S = State{};
    K = Pipe{};
    sky_gl11_reset();
    swap_interval_reset();
}

static bool is_core() { return S.core; }
static bool alive() { return S.wnd != nullptr; }

/* Lights are constant for the frame. Per-draw uploads were most of the cost
   of the floor grid, which is one segment at a time. */
static void upload_lights() {
    K.P.Use(K.prog);
    K.P.Uni3fv(K.uAmb, 1, K.amb);
    if (K.uFill >= 0) K.P.Uni1f(K.uFill, K.fill);
    K.P.Uni1i(K.uLN, K.lights);
    if (K.uPos >= 0) K.P.Uni4fv(K.uPos, 16, K.lpos);
    if (K.uDif >= 0) K.P.Uni3fv(K.uDif, 16, K.ldif);
    if (K.uSpot >= 0) K.P.Uni3fv(K.uSpot, 16, K.lspot);
    if (K.uSp >= 0) K.P.Uni3fv(K.uSp, 16, K.lsp);
}

/* Distance is scene pixels. The shader works in render units, where 32
   scene pixels are 1. A zero distance leaves the picture clear. */
static void fog_publish() {
    float col[3] = { g_fogR / 255.0f, g_fogG / 255.0f, g_fogB / 255.0f };
    float dist = (g_fog && g_fogDist > 0) ? g_fogDist * kUnit : 0.f;
    K.P.Use(K.prog);
    if (K.uFogColor >= 0) K.P.Uni3fv(K.uFogColor, 1, col);
    if (K.uFogDist >= 0) K.P.Uni1f(K.uFogDist, dist);
    if (K.matProg) {
        K.P.Use(K.matProg);
        if (K.muFogColor >= 0) K.P.Uni3fv(K.muFogColor, 1, col);
        if (K.muFogDist >= 0) K.P.Uni1f(K.muFogDist, dist);
        K.P.Use(K.prog);
    }
}

/* The vertex format never changes (position, normal, color). One VAO per
   buffer keeps that layout, so a draw does not rebind the attributes. */
static void bind_mesh(GLuint vbo) {
    auto it = K.meshVao.find(vbo);
    if (it != K.meshVao.end()) {
        K.P.BindVao(it->second);
        return;
    }
    GLuint vao = 0;
    K.P.GenVao(1, &vao);
    if (!vao) {
        K.P.BindVao(K.vao);
        K.P.BindBuffer(kArray, vbo);
        const GLsizei stride = (GLsizei)(9 * sizeof(float));
        K.P.EnableAttrib(0);
        K.P.AttribPtr(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
        K.P.EnableAttrib(1);
        K.P.AttribPtr(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
        K.P.EnableAttrib(2);
        K.P.AttribPtr(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
        return;
    }
    K.P.BindVao(vao);
    K.P.BindBuffer(kArray, vbo);
    const GLsizei stride = (GLsizei)(9 * sizeof(float));
    K.P.EnableAttrib(0);
    K.P.AttribPtr(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    K.P.EnableAttrib(1);
    K.P.AttribPtr(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    K.P.EnableAttrib(2);
    K.P.AttribPtr(2, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
    K.meshVao.emplace(vbo, vao);
}

static void send_shade() {
    if (g_sentLit != K.lit) { K.P.Uni1f(K.uLit, K.lit); g_sentLit = K.lit; }
    if (g_sentTwo != K.two) { K.P.Uni1f(K.uTwo, K.two); g_sentTwo = K.two; }
    if (g_sentVert != K.vert) { K.P.Uni1f(K.uVert, K.vert); g_sentVert = K.vert; }
    if (g_sentAlpha != K.alpha) { K.P.Uni1f(K.uAlpha, K.alpha); g_sentAlpha = K.alpha; }
    if (g_sentNdcX != K.ndcX || g_sentNdcY != K.ndcY) {
        K.P.Uni2f(K.uNdc, K.ndcX, K.ndcY);
        g_sentNdcX = K.ndcX;
        g_sentNdcY = K.ndcY;
    }
}

static void draw_mesh(GLuint vbo, GLsizei n, const float* mvp, const float* mv,
                      GLenum mode = GL_TRIANGLES) {
    if (!vbo || n <= 0) return;
    float nm[9];
    normal_matrix(mv, nm);
    K.P.Use(K.prog);
    K.P.UniMat4(K.uMvp, 1, GL_FALSE, mvp);
    K.P.UniMat4(K.uMv, 1, GL_FALSE, mv);
    if (K.uLightMvp >= 0) K.P.UniMat4(K.uLightMvp, 1, GL_FALSE, K.lightMvp);
    K.P.UniMat3(K.uN, 1, GL_FALSE, nm);
    K.P.Uni3fv(K.uColor, 1, K.color);
    send_shade();
    bind_mesh(vbo);
    glDrawArrays(mode, 0, n);
}

static bool model_has_maps(const Model& m) {
    return m.base.w > 0 || m.nrm.w > 0 || m.metal.w > 0 || m.rough.w > 0 || m.height.w > 0;
}

static GLuint upload_map(const MapImg& im) {
    if (im.w < 1 || im.rgb.empty()) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    if (!id) return 0;
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, im.w, im.h, 0, GL_RGB, GL_UNSIGNED_BYTE, im.rgb.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return id;
}

static int model_map_bits(const Model& m) {
    int bits = 0;
    if (m.base.w > 0) bits |= 1;
    if (m.nrm.w > 0) bits |= 2;
    if (m.metal.w > 0) bits |= 4;
    if (m.rough.w > 0) bits |= 8;
    if (m.height.w > 0) bits |= 16;
    return bits;
}

static GLuint model_mat_vbo(const Model& m, GLsizei* n) {
    auto it = K.matMesh.find(&m);
    if (it != K.matMesh.end()) { *n = it->second.second; return it->second.first; }
    std::vector<float> v;
    v.reserve(m.tri.size() * 12);
    for (size_t k = 0; k + 2 < m.tri.size(); k += 3) {
        V3 p[3], nm[3];
        float u[3], vv[3];
        for (int c = 0; c < 3; ++c) {
            p[c] = m.pos[m.tri[k + c]];
            nm[c] = (k + c < m.cn.size() && dot(m.cn[k + c], m.cn[k + c]) > 1e-8f)
                ? norm(m.cn[k + c]) : v3(0, 0, 0);
            u[c] = (k + c) * 2 + 1 < m.cuv.size() ? m.cuv[(k + c) * 2] : 0;
            vv[c] = (k + c) * 2 + 1 < m.cuv.size() ? m.cuv[(k + c) * 2 + 1] : 0;
        }
        V3 face = norm(cross(p[1] - p[0], p[2] - p[0]));
        V3 e1 = p[1] - p[0], e2 = p[2] - p[0];
        float du1 = u[1] - u[0], dv1 = vv[1] - vv[0];
        float du2 = u[2] - u[0], dv2 = vv[2] - vv[0];
        float det = du1 * dv2 - du2 * dv1;
        V3 tan = v3(1, 0, 0), bit = v3(0, 1, 0);
        if (std::fabs(det) > 1e-8f) {
            float r = 1.0f / det;
            tan = norm((e1 * dv2 - e2 * dv1) * r);
            bit = (e2 * du1 - e1 * du2) * r;
        }
        float hand = dot(cross(face, tan), bit) < 0.f ? -1.f : 1.f;
        for (int c = 0; c < 3; ++c) {
            V3 nrm = dot(nm[c], nm[c]) > 0.5f ? nm[c] : face;
            V3 t = norm(tan - nrm * dot(nrm, tan));
            v.push_back(p[c].x); v.push_back(p[c].y); v.push_back(p[c].z);
            v.push_back(nrm.x); v.push_back(nrm.y); v.push_back(nrm.z);
            v.push_back(t.x); v.push_back(t.y); v.push_back(t.z); v.push_back(hand);
            v.push_back(u[c]); v.push_back(vv[c]);
        }
    }
    GLuint id = 0;
    GLsizei count = (GLsizei)(v.size() / 12);
    if (!v.empty()) {
        K.P.GenBuffers(1, &id);
        K.P.BindBuffer(kArray, id);
        K.P.BufferData(kArray, (std::intptr_t)(v.size() * sizeof(float)), v.data(), kStatic);
        GLuint vao = 0;
        K.P.GenVao(1, &vao);
        K.P.BindVao(vao);
        K.P.BindBuffer(kArray, id);
        const GLsizei stride = (GLsizei)(12 * sizeof(float));
        K.P.EnableAttrib(0);
        K.P.AttribPtr(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
        K.P.EnableAttrib(1);
        K.P.AttribPtr(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
        K.P.EnableAttrib(2);
        K.P.AttribPtr(2, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
        K.P.EnableAttrib(3);
        K.P.AttribPtr(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(10 * sizeof(float)));
        K.P.BindVao(0);
        K.matVao[&m] = vao;
    }
    K.matMesh[&m] = { id, count };
    *n = count;
    return id;
}

static void draw_model_maps(const Model& m, GLsizei n, const float* mvp, const float* mv) {
    if (!K.matProg || !K.P.ActiveTex || n <= 0) return;
    GLuint vbo = model_mat_vbo(m, &n);
    auto vit = K.matVao.find(&m);
    if (!vbo || vit == K.matVao.end()) return;
    struct MapIds { GLuint id[5]; };
    static std::unordered_map<const Model*, MapIds> maps;
    if (!maps.count(&m)) {
        MapIds set{};
        set.id[0] = upload_map(m.base);
        set.id[1] = upload_map(m.nrm);
        set.id[2] = upload_map(m.metal);
        set.id[3] = upload_map(m.rough);
        set.id[4] = upload_map(m.height);
        maps[&m] = set;
    }
    const GLuint* ids = maps[&m].id;
    float nm[9];
    normal_matrix(mv, nm);
    K.P.Use(K.matProg);
    K.P.UniMat4(K.muMvp, 1, GL_FALSE, mvp);
    K.P.UniMat4(K.muMv, 1, GL_FALSE, mv);
    if (K.muLightMvp >= 0) K.P.UniMat4(K.muLightMvp, 1, GL_FALSE, K.lightMvp);
    K.P.UniMat3(K.muN, 1, GL_FALSE, nm);
    if (K.muColor >= 0) K.P.Uni3fv(K.muColor, 1, K.color);
    if (K.muAmb >= 0) K.P.Uni3fv(K.muAmb, 1, K.amb);
    if (K.muFill >= 0) K.P.Uni1f(K.muFill, K.fill);
    if (K.muLit >= 0) K.P.Uni1f(K.muLit, K.lit);
    if (K.muAlpha >= 0) K.P.Uni1f(K.muAlpha, K.alpha);
    if (K.muNdc >= 0) K.P.Uni2f(K.muNdc, K.ndcX, K.ndcY);
    if (K.muLN >= 0) K.P.Uni1i(K.muLN, K.lights);
    if (K.muMap >= 0) K.P.Uni1i(K.muMap, model_map_bits(m));
    if (K.muPos >= 0) K.P.Uni4fv(K.muPos, 16, K.lpos);
    if (K.muDif >= 0) K.P.Uni3fv(K.muDif, 16, K.ldif);
    if (K.muSpot >= 0) K.P.Uni3fv(K.muSpot, 16, K.lspot);
    if (K.muSp >= 0) K.P.Uni3fv(K.muSp, 16, K.lsp);
    for (int i = 0; i < 5; ++i) {
        K.P.ActiveTex(0x84C0 + i);
        glBindTexture(GL_TEXTURE_2D, ids[i]);
    }
    K.P.BindVao(vit->second);
    glDrawArrays(GL_TRIANGLES, 0, n);
    for (int i = 0; i < 5; ++i) {
        K.P.ActiveTex(0x84C0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    K.P.ActiveTex(0x84C0);
    K.P.Use(K.prog);
}

/* World matrix and the unit mesh. mapped is set when the color pass should
   use the textured material shader. */
static bool mesh_local(const Entity& e, float* shape, GLuint* vbo, GLsizei* n, float* two, const Model** mapped) {
    float w = (float)e.w, h = (float)e.h, d = (float)depth_of(e);
    if (w <= 0 || h <= 0) return false;
    float sx, sy, sz;
    *vbo = K.vboBox;
    *n = K.nBox;
    *two = 0;
    if (mapped) *mapped = nullptr;
    if (e.kind == "sphere") {
        float dm = std::min(w, h) * kUnit;
        sx = sy = sz = dm;
        *vbo = K.vboSphere; *n = K.nSphere;
    } else if (e.kind == "cylinder") {
        sx = w * kUnit; sy = h * kUnit; sz = w * kUnit;
        *vbo = K.vboCyl; *n = K.nCyl;
    } else if (e.kind == "capsule") {
        sx = w * kUnit; sy = h * kUnit; sz = w * kUnit;
        CapsuleLocal cap = capsule_local(w, h);
        static std::vector<float> verts;
        verts.clear();
        capsule_emit(cap, sx, sy, sz, [&](float px, float py, float pz, float nx, float ny, float nz) {
            push9(verts, px, py, pz, nx, ny, nz, 1, 1, 1);
        });
        if (!K.vboCap) K.P.GenBuffers(1, &K.vboCap);
        K.P.BindBuffer(kArray, K.vboCap);
        K.P.BufferData(kArray, (std::intptr_t)(verts.size() * sizeof(float)), verts.data(), kDynamic);
        *vbo = K.vboCap;
        *n = (GLsizei)(verts.size() / 9);
    } else {
        const Model* m = nullptr;
        if (e.kind == "model" && !e.model.empty()) m = &model_load(model_path(e.model));
        if (m && m->ok) {
            float s = std::max(w, h) * kUnit;
            sx = sy = sz = s;
            *vbo = model_vbo(*m, n);
            *two = 1;
            if (mapped && model_has_maps(*m) && K.matProg && K.lit > 0.5f) *mapped = m;
        } else {
            sx = w * kUnit; sy = h * kUnit; sz = d * kUnit;
        }
    }
    float t[16], r[16], sc[16], tr[16];
    ident(t); ident(sc);
    t[12] = (e.x + w * 0.5f) * kUnit;
    t[13] = -(e.y + h * 0.5f) * kUnit;
    t[14] = -(e.z + d * 0.5f) * kUnit;
    ::orient_matrix(r, e.rotX, e.rotY, e.rotDeg);
    sc[0] = sx; sc[5] = sy; sc[10] = sz;
    mul(tr, t, r);
    mul(shape, tr, sc);
    return true;
}

static void shape33(const Entity& e) {
    float shape[16];
    GLuint vbo = 0;
    GLsizei n = 0;
    float two = 0;
    const Model* mapped = nullptr;
    if (!mesh_local(e, shape, &vbo, &n, &two, &mapped)) return;
    float mv[16], mvp[16];
    mul(mv, K.cam, shape);
    mul(mvp, K.proj, mv);
    if (K.shadowOn) mul(K.lightMvp, K.lightVP, shape);
    else ident(K.lightMvp);
    float saved = K.two;
    K.two = two;
    if (mapped) draw_model_maps(*mapped, n, mvp, mv);
    else draw_mesh(vbo, n, mvp, mv);
    K.two = saved;
}

static void add_light(int slot, const float* eye, float r, float g, float b,
                      const float* spot, float cutoffCos, float exponent, float attenQ) {
    if (slot < 0 || slot >= 16) return;
    K.lpos[slot * 4 + 0] = eye[0];
    K.lpos[slot * 4 + 1] = eye[1];
    K.lpos[slot * 4 + 2] = eye[2];
    K.lpos[slot * 4 + 3] = eye[3];
    K.ldif[slot * 3 + 0] = r;
    K.ldif[slot * 3 + 1] = g;
    K.ldif[slot * 3 + 2] = b;
    K.lspot[slot * 3 + 0] = spot[0];
    K.lspot[slot * 3 + 1] = spot[1];
    K.lspot[slot * 3 + 2] = spot[2];
    K.lsp[slot * 3 + 0] = cutoffCos;
    K.lsp[slot * 3 + 1] = exponent;
    K.lsp[slot * 3 + 2] = attenQ;
}

static bool draw_sky33(const float* basis) {
    if (!K.skyProg || !K.skyVao || !K.P.ActiveTex) return false;
    const SkyCpu* cpu = sky_cpu();
    if (!cpu || cpu->face[0].w < 1) return false;
    if (K.skyStamp != cpu->stamp || !K.skyTex[0]) {
        if (!K.skyTex[0]) glGenTextures(6, K.skyTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (int i = 0; i < 6; ++i) {
            const SkyImg& im = cpu->face[i];
            if (im.w < 1 || im.rgb.empty()) return false;
            K.P.ActiveTex(0x84C0 + i);
            glBindTexture(GL_TEXTURE_2D, K.skyTex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, im.w, im.h, 0, GL_RGB, GL_UNSIGNED_BYTE, im.rgb.data());
        }
        K.skyStamp = cpu->stamp;
    }
    float view[16];
    mul(view, K.proj, basis);
    glDepthMask(GL_FALSE);
    K.P.Use(K.skyProg);
    K.P.UniMat4(K.skyUView, 1, GL_FALSE, view);
    if (K.skyUFog >= 0) {
        float col[3] = { g_fogR / 255.0f, g_fogG / 255.0f, g_fogB / 255.0f };
        K.P.Uni3fv(K.skyUFog, 1, col);
    }
    if (K.skyUFogOn >= 0) K.P.Uni1f(K.skyUFogOn, g_fog ? 1.f : 0.f);
    for (int i = 0; i < 6; ++i) {
        K.P.ActiveTex(0x84C0 + i);
        glBindTexture(GL_TEXTURE_2D, K.skyTex[i]);
        if (K.skyUFace[i] >= 0) K.P.Uni1i(K.skyUFace[i], i);
    }
    K.P.BindVao(K.skyVao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    K.P.ActiveTex(0x84C0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
    return true;
}

using FbTexFn = void (APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
static FbTexFn g_fbTex = nullptr;
static int g_shFailed = 0;

static constexpr int kShadowMap = 4096;

static bool ensure_shadow_target() {
    if (g_shFailed) return false;
    if (K.shFbo && K.shTex) return true;
    if (!g_fbTex) g_fbTex = (FbTexFn)proc("glFramebufferTexture2D");
    if (!g_fbTex || !S.genFb || !S.bindFb || !S.status) return false;
    glGenTextures(1, &K.shTex);
    glBindTexture(GL_TEXTURE_2D, K.shTex);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6, kShadowMap, kShadowMap, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812D);
    float border[4] = { 1, 1, 1, 1 };
    glTexParameterfv(GL_TEXTURE_2D, 0x1004, border);
    glBindTexture(GL_TEXTURE_2D, 0);
    S.genFb(1, &K.shFbo);
    S.bindFb(kFramebuffer, K.shFbo);
    g_fbTex(kFramebuffer, kDepthAttachment, GL_TEXTURE_2D, K.shTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    bool ok = S.status(kFramebuffer) == kComplete;
    S.bindFb(kFramebuffer, S.fbo);
    if (!ok) { g_shFailed = 1; return false; }
    return true;
}

/* Fit the light box to the camera so a huge ground plane cannot dilute the
   shadow map into blocky texels. */
static void shadow_fit_box(V3& center, float& half, float sceneRadius) {
    float halfScene = sceneRadius * 1.1f;
    if (halfScene < 6.f) halfScene = 6.f;
    if (!C.valid) {
        half = halfScene;
        if (half > 24.f) half = 24.f;
        return;
    }
    V3 gap = C.target - C.eye;
    float d = std::sqrt(dot(gap, gap));
    float tanHalf = (T.h * 0.5f) / std::max(C.f, 1.f);
    float aspect = (float)T.w / (float)std::max(T.h, 1);
    float halfCam = d * tanHalf * std::max(aspect, 1.f) * 1.35f + 4.f;
    if (halfCam < 10.f) halfCam = 10.f;
    if (halfCam > 24.f) halfCam = 24.f;
    half = halfCam;
    if (halfScene < half) half = std::max(halfScene, 8.f);
    center = C.target;
}

/* Large flat floors inflate the light box; skip them when measuring casters. */
static bool shadow_is_floor(float c0, float c1, float c2) {
    float a = c0, b = c1, c = c2;
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return c > 18.f && a < c * 0.08f;
}

static void shadow_publish(int on, int sunSlot) {
    K.shadowOn = on;
    K.P.Use(K.prog);
    if (K.uDoShadow >= 0) K.P.Uni1f(K.uDoShadow, on ? 1.f : 0.f);
    if (K.uSun >= 0) K.P.Uni1i(K.uSun, on ? sunSlot : -1);
    if (K.uShadowMap >= 0) K.P.Uni1i(K.uShadowMap, 7);
    if (K.matProg) {
        K.P.Use(K.matProg);
        if (K.muDoShadow >= 0) K.P.Uni1f(K.muDoShadow, on ? 1.f : 0.f);
        if (K.muSun >= 0) K.P.Uni1i(K.muSun, on ? sunSlot : -1);
        if (K.muShadowMap >= 0) K.P.Uni1i(K.muShadowMap, 7);
    }
    K.P.Use(K.prog);
}

/* One depth picture from the directional light, covering every solid mesh. */
static void shadow_frame(V3 shine, int sunSlot, int viewW, int viewH) {
    ident(K.lightVP);
    ident(K.lightMvp);
    if (!g_shadows || sunSlot < 0 || !K.shProg || !ensure_shadow_target()) {
        shadow_publish(0, -1);
        return;
    }
    int casters = 0;
    float minx = 1e9f, miny = 1e9f, minz = 1e9f;
    float maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        const Entity& e = g.entities[s];
        if (e.a <= 8 || !aa_kind(e) || !entity_shown(s)) continue;
        float shape[16];
        GLuint vbo = 0;
        GLsizei n = 0;
        float two = 0;
        if (!mesh_local(e, shape, &vbo, &n, &two, nullptr) || !vbo || n <= 0) continue;
        float c0 = std::sqrt(shape[0] * shape[0] + shape[1] * shape[1] + shape[2] * shape[2]);
        float c1 = std::sqrt(shape[4] * shape[4] + shape[5] * shape[5] + shape[6] * shape[6]);
        float c2 = std::sqrt(shape[8] * shape[8] + shape[9] * shape[9] + shape[10] * shape[10]);
        if (shadow_is_floor(c0, c1, c2)) continue;
        float rad = 0.5f * std::sqrt(c0 * c0 + c1 * c1 + c2 * c2);
        float x = shape[12], y = shape[13], z = shape[14];
        if (x - rad < minx) minx = x - rad;
        if (y - rad < miny) miny = y - rad;
        if (z - rad < minz) minz = z - rad;
        if (x + rad > maxx) maxx = x + rad;
        if (y + rad > maxy) maxy = y + rad;
        if (z + rad > maxz) maxz = z + rad;
        casters++;
    }
    V3 center = C.valid ? C.target : v3(0, 0, 0);
    float radius = 8.f;
    if (casters > 0) {
        V3 ext = v3(maxx - minx, maxy - miny, maxz - minz) * 0.5f;
        radius = std::sqrt(dot(ext, ext));
        if (radius < 0.5f) radius = 0.5f;
        center = v3((minx + maxx) * 0.5f, (miny + maxy) * 0.5f, (minz + maxz) * 0.5f);
    } else if (!C.valid) {
        shadow_publish(0, -1);
        return;
    }
    float half = radius * 1.1f;
    shadow_fit_box(center, half, radius);
    V3 fwd = norm(shine);
    V3 upRef = std::fabs(fwd.y) > 0.95f ? v3(0, 0, 1) : v3(0, 1, 0);
    V3 right = norm(cross(fwd, upRef));
    V3 up = norm(cross(right, fwd));
    float dist = half + 4.0f;
    V3 eye = center - fwd * dist;
    /* Snap the light origin to the shadow texel grid so edges do not crawl. */
    float texel = (half * 2.f) / (float)kShadowMap;
    if (texel > 1e-6f) {
        float lx = dot(eye, right);
        float ly = dot(eye, up);
        lx = std::floor(lx / texel) * texel;
        ly = std::floor(ly / texel) * texel;
        float lz = dot(eye, fwd);
        eye = right * lx + up * ly + fwd * lz;
    }
    float basis[16] = {
        right.x, up.x, -fwd.x, 0,
        right.y, up.y, -fwd.y, 0,
        right.z, up.z, -fwd.z, 0,
        0, 0, 0, 1,
    };
    float tr[16], view[16], ortho[16];
    ident(tr);
    tr[12] = -eye.x; tr[13] = -eye.y; tr[14] = -eye.z;
    mul(view, basis, tr);
    float z0 = 0.05f, z1 = dist + half + 4.0f;
    std::memset(ortho, 0, sizeof(ortho));
    ortho[0] = 1.0f / half;
    ortho[5] = 1.0f / half;
    ortho[10] = -2.0f / (z1 - z0);
    ortho[14] = -(z1 + z0) / (z1 - z0);
    ortho[15] = 1;
    mul(K.lightVP, ortho, view);

    S.bindFb(kFramebuffer, K.shFbo);
    glViewport(0, 0, kShadowMap, kShadowMap);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    K.P.Use(K.shProg);
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        const Entity& e = g.entities[s];
        if (e.a <= 8 || !aa_kind(e) || !entity_shown(s)) continue;
        float shape[16], mvp[16];
        GLuint vbo = 0;
        GLsizei n = 0;
        float two = 0;
        if (!mesh_local(e, shape, &vbo, &n, &two, nullptr) || !vbo || n <= 0) continue;
        mul(mvp, K.lightVP, shape);
        K.P.UniMat4(K.shUMvp, 1, GL_FALSE, mvp);
        bind_mesh(vbo);
        glDrawArrays(GL_TRIANGLES, 0, n);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_CULL_FACE);
    if (K.P.ActiveTex) {
        K.P.ActiveTex(0x84C0 + 7);
        glBindTexture(GL_TEXTURE_2D, K.shTex);
        K.P.ActiveTex(0x84C0);
    }
    shadow_publish(1, sunSlot);
    S.bindFb(kFramebuffer, S.fbo);
    glViewport(0, 0, viewW, viewH);
}

static bool begin33(int w, int h, double fov_deg, int br, int bg, int bb) {
    if (!wglMakeCurrent(S.dc, S.rc) || !ensure_buffers(w, h, aa_samples())) return false;
    g_lines.clear();
    g_lineA = -1;
    g_sentLit = g_sentTwo = g_sentVert = g_sentAlpha = -1;
    g_sentNdcX = g_sentNdcY = 999;
    S.selected.clear();
    S.bindFb(kFramebuffer, S.fbo);
    glViewport(0, 0, w, h);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    aa_project();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CW);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    const double zn = kNear, zf = 4000.0;
    const double top = zn * std::tan(fov_deg * 0.5 * 3.14159265358979 / 180.0);
    const double right = top * (double)w / (double)h;
    const double rl = right - (-right), tb = top - (-top), fn = zf - zn;
    std::memset(K.proj, 0, sizeof(K.proj));
    K.proj[0] = (float)(2 * zn / rl);
    K.proj[5] = (float)(-2 * zn / tb);
    K.proj[10] = (float)(-(zf + zn) / fn);
    K.proj[11] = -1;
    K.proj[14] = (float)(-2 * zf * zn / fn);

    float basis[16] = {
        C.right.x, C.up.x, -C.fwd.x, 0,
        C.right.y, C.up.y, -C.fwd.y, 0,
        C.right.z, C.up.z, -C.fwd.z, 0,
        0, 0, 0, 1,
    };
    if (!draw_sky33(basis)) {
        float idm[16];
        ident(idm);
        int cr = br, cg = bg, cb = bb;
        if (g_fog) { cr = g_fogR; cg = g_fogG; cb = g_fogB; }
        int r0 = g_fog ? cr : std::min(255, cr + 14);
        int g0 = g_fog ? cg : std::min(255, cg + 16);
        int b0 = g_fog ? cb : std::min(255, cb + 22);
        float lr = r0 / 255.0f, lg = g0 / 255.0f, lb = b0 / 255.0f;
        float dr = cr / 255.0f, dg = cg / 255.0f, db = cb / 255.0f;
        float bgv[6 * 9];
        auto put = [&](int i, float x, float y, float rr, float gg, float bbv) {
            float* p = bgv + i * 9;
            p[0] = x; p[1] = y; p[2] = 0; p[3] = p[4] = 0; p[5] = 1;
            p[6] = rr; p[7] = gg; p[8] = bbv;
        };
        put(0, -1, -1, lr, lg, lb); put(1, 1, -1, lr, lg, lb); put(2, 1, 1, dr, dg, db);
        put(3, -1, -1, lr, lg, lb); put(4, 1, 1, dr, dg, db); put(5, -1, 1, dr, dg, db);
        K.P.BindBuffer(kArray, K.vboBg);
        K.P.BufferData(kArray, (std::intptr_t)sizeof(bgv), bgv, kDynamic);
        K.lit = 0; K.vert = 1; K.alpha = 1; K.two = 0; K.ndcX = K.ndcY = 0;
        draw_mesh(K.vboBg, 6, idm, idm);
    }
    float tr[16];
    ident(tr);
    tr[12] = -C.eye.x; tr[13] = -C.eye.y; tr[14] = -C.eye.z;
    mul(K.cam, basis, tr);

    std::memset(K.lpos, 0, sizeof(K.lpos));
    std::memset(K.ldif, 0, sizeof(K.ldif));
    std::memset(K.lspot, 0, sizeof(K.lspot));
    for (int i = 0; i < 16; ++i) {
        K.lpos[i * 4 + 2] = 1;
        K.lspot[i * 3 + 2] = -1;
        K.lsp[i * 3] = 2;
    }
    K.amb[0] = K.amb[1] = K.amb[2] = 0.55f;
    bool any_ambient = false, any_sun = false;
    for (const Light& l : g_lights) if (l.type == 0) any_sun = true;
    int n = 0;
    const float noSpot[3] = { 0, 0, -1 };
    auto put_dir = [&](int slot, float x, float y, float z, float r, float g, float b) {
        float world[4] = { x, y, z, 0 }, eye[4];
        xform(K.cam, world[0], world[1], world[2], 0, eye);
        eye[3] = 0;
        add_light(slot, eye, r, g, b, noSpot, 2, 0, 0);
    };
    /* A scene that already placed lamps keeps those pools. The standby sun
       would wash the whole room and flip it bright or black as you turn. */
    bool has_lamp = false;
    for (const Light& l : g_lights) if (l.type == 1 || l.type == 2) has_lamp = true;
    int sunSlot = -1;
    V3 shine = kDefaultLight * -1.f;
    if (!any_sun && !has_lamp) {
        put_dir(n++, kDefaultLight.x, kDefaultLight.y, kDefaultLight.z, 0.95f, 0.95f, 0.95f);
        sunSlot = n - 1;
    }
    for (const Light& l : g_lights) {
        if (l.type == 3) {
            if (!any_ambient) { K.amb[0] = K.amb[1] = K.amb[2] = 0; any_ambient = true; }
            K.amb[0] += l.r; K.amb[1] += l.g; K.amb[2] += l.b;
            continue;
        }
        if (n >= 16) continue;
        if (l.type == 0) {
            int slot = n;
            put_dir(n++, -l.dir.x, -l.dir.y, -l.dir.z, l.r, l.g, l.b);
            if (sunSlot < 0) { sunSlot = slot; shine = l.dir; }
        } else {
            float eye[4], sd[4];
            xform(K.cam, l.pos.x, l.pos.y, l.pos.z, 1, eye);
            eye[3] = 1;
            xform(K.cam, l.dir.x, l.dir.y, l.dir.z, 0, sd);
            float spot[3] = { sd[0], sd[1], sd[2] };
            float cutoff = 2, expo = 0, q = 0;
            q = 1.0f / (l.range * l.range);
            if (l.type == 2) {
                float deg = std::clamp(l.cone * 0.5f, 1.0f, 90.0f);
                cutoff = std::cos(deg * 3.14159265f / 180.0f);
                expo = 6;
            }
            add_light(n++, eye, l.r, l.g, l.b, spot, cutoff, expo, q);
        }
    }
    K.lights = n;
    K.lit = 1; K.vert = 0; K.two = 0; K.alpha = 1;
    K.fill = any_ambient ? 1.0f : 0.0f;
    upload_lights();
    fog_publish();
    shadow_frame(shine, sunSlot, w, h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    return true;
}

static void flush_lines33() {
    if (g_lines.empty()) return;
    K.P.BindBuffer(kArray, K.vboLine);
    K.P.BufferData(kArray, (std::intptr_t)(g_lines.size() * sizeof(float)), g_lines.data(), kDynamic);
    K.lit = 0; K.vert = 1; K.two = 0;
    K.alpha = std::clamp(g_lineA, 0, 255) / 255.0f;
    K.ndcX = K.ndcY = 0;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glLineWidth(1.0f);
    float mvp[16];
    mul(mvp, K.proj, K.cam);
    draw_mesh(K.vboLine, (GLsizei)(g_lines.size() / 9), mvp, K.cam, GL_LINES);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    K.lit = 1; K.vert = 0; K.alpha = 1;
    g_lines.clear();
    g_lineA = -1;
}

static void entity33(const Entity& e, int slot) {
    flush_lines33();
    int aa = std::clamp(e.a, 0, 255);
    K.color[0] = e.r / 255.0f; K.color[1] = e.g / 255.0f; K.color[2] = e.b / 255.0f;
    K.lit = 1; K.vert = 0; K.alpha = aa / 255.0f; K.ndcX = K.ndcY = 0;
    if (aa < 255) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    /* A held prop stays in the near depth band so it does not slice into the
       walls. A little fill keeps its color map visible; the cone still shades it. */
    bool held = slot == g_viewSlot;
    float amb0 = K.amb[0], amb1 = K.amb[1], amb2 = K.amb[2];
    if (held) {
        K.amb[0] = std::min(1.f, K.amb[0] + 0.28f);
        K.amb[1] = std::min(1.f, K.amb[1] + 0.24f);
        K.amb[2] = std::min(1.f, K.amb[2] + 0.18f);
        glDepthRange(0.0, 0.18);
    }
    shape33(e);
    if (held) {
        K.amb[0] = amb0;
        K.amb[1] = amb1;
        K.amb[2] = amb2;
        glDepthRange(0.0, 1.0);
    }
    if (aa < 255) glDisable(GL_BLEND);
    if (e.selected) S.selected.push_back(slot);
}

static void cover33(const V3* pts, int ntri, int r, int g, int b, int alpha) {
    if (!pts || ntri <= 0) return;
    flush_lines33();
    g_scratch.clear();
    g_scratch.reserve((size_t)ntri * 27);
    float cr = r / 255.0f, cg = g / 255.0f, cb = b / 255.0f;
    for (int i = 0; i < ntri; ++i) {
        const V3& p0 = pts[i * 3];
        const V3& p1 = pts[i * 3 + 1];
        const V3& p2 = pts[i * 3 + 2];
        V3 n = norm(cross(p1 - p0, p2 - p0));
        push9(g_scratch, p0.x, p0.y, p0.z, n.x, n.y, n.z, cr, cg, cb);
        push9(g_scratch, p1.x, p1.y, p1.z, n.x, n.y, n.z, cr, cg, cb);
        push9(g_scratch, p2.x, p2.y, p2.z, n.x, n.y, n.z, cr, cg, cb);
    }
    K.P.BindBuffer(kArray, K.vboLine);
    K.P.BufferData(kArray, (std::intptr_t)(g_scratch.size() * sizeof(float)), g_scratch.data(), kDynamic);
    K.lit = 0; K.vert = 1; K.two = 0;
    K.alpha = std::clamp(alpha, 0, 255) / 255.0f;
    K.ndcX = K.ndcY = 0;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    float mvp[16];
    mul(mvp, K.proj, K.cam);
    draw_mesh(K.vboLine, (GLsizei)(g_scratch.size() / 9), mvp, K.cam, GL_TRIANGLES);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    K.lit = 1; K.vert = 0; K.alpha = 1;
}

/* The model's own triangles, not a snapped stand-in. */
static void cover_model33(const Entity& e, int r, int g, int b, int alpha) {
    const Model* m = nullptr;
    if (!e.model.empty()) m = &model_load(model_path(e.model));
    if (!m || !m->ok) return;
    float w = (float)e.w, h = (float)e.h, d = (float)depth_of(e);
    if (w <= 0 || h <= 0) return;
    GLsizei n = 0;
    GLuint vbo = model_vbo(*m, &n);
    if (!vbo || n <= 0) return;
    flush_lines33();
    float sz = std::max(w, h) * kUnit;
    float t[16], rm[16], sc[16], tr[16], shape[16], mv[16], mvp[16];
    ident(t); ident(sc);
    t[12] = (e.x + w * 0.5f) * kUnit;
    t[13] = -(e.y + h * 0.5f) * kUnit;
    t[14] = -(e.z + d * 0.5f) * kUnit;
    ::orient_matrix(rm, e.rotX, e.rotY, e.rotDeg);
    sc[0] = sc[5] = sc[10] = sz;
    mul(tr, t, rm);
    mul(shape, tr, sc);
    mul(mv, K.cam, shape);
    mul(mvp, K.proj, mv);
    K.color[0] = r / 255.0f; K.color[1] = g / 255.0f; K.color[2] = b / 255.0f;
    K.lit = 0; K.vert = 0; K.two = 1;
    K.alpha = std::clamp(alpha, 0, 255) / 255.0f;
    K.ndcX = K.ndcY = 0;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    draw_mesh(vbo, n, mvp, mv);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    K.lit = 1; K.vert = 0; K.two = 0; K.alpha = 1;
}

static void painted33(const float* v, int ntri) {
    if (!v || ntri <= 0) return;
    flush_lines33();
    g_scratch.clear();
    g_scratch.reserve((size_t)ntri * 27);
    for (int i = 0; i < ntri * 3; ++i) {
        const float* p = v + i * 6;
        push9(g_scratch, p[0], p[1], p[2], 0, 0, 1, p[3], p[4], p[5]);
    }
    K.P.BindBuffer(kArray, K.vboLine);
    K.P.BufferData(kArray, (std::intptr_t)(g_scratch.size() * sizeof(float)), g_scratch.data(), kDynamic);
    K.lit = 0; K.vert = 1; K.two = 0;
    K.alpha = 1;
    K.ndcX = K.ndcY = 0;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    float mvp[16];
    mul(mvp, K.proj, K.cam);
    draw_mesh(K.vboLine, (GLsizei)(g_scratch.size() / 9), mvp, K.cam, GL_TRIANGLES);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    K.lit = 1; K.vert = 0; K.alpha = 1;
}

static void sector33(const V3* pts, int ntri, int r, int g, int b, int a) {
    if (!pts || ntri <= 0) return;
    flush_lines33();
    g_scratch.clear();
    g_scratch.reserve((size_t)ntri * 27);
    float cr = r / 255.0f, cg = g / 255.0f, cb = b / 255.0f;
    for (int i = 0; i < ntri * 3; ++i) {
        const V3& p = pts[i];
        push9(g_scratch, p.x, p.y, p.z, 0, 0, 1, cr, cg, cb);
    }
    K.P.BindBuffer(kArray, K.vboLine);
    K.P.BufferData(kArray, (std::intptr_t)(g_scratch.size() * sizeof(float)), g_scratch.data(), kDynamic);
    K.lit = 0; K.vert = 1; K.two = 0;
    K.alpha = std::clamp(a, 0, 255) / 255.0f;
    K.ndcX = K.ndcY = 0;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float mvp[16];
    mul(mvp, K.proj, K.cam);
    draw_mesh(K.vboLine, (GLsizei)(g_scratch.size() / 9), mvp, K.cam, GL_TRIANGLES);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    K.lit = 1; K.vert = 0; K.alpha = 1;
}

/* x0,y0,x1,y1 are NDC. y = -1 is the top of the picture. */
static void screen_rect33(float x0, float y0, float x1, float y1, int r, int g, int b, int a) {
    flush_lines33();
    a = std::clamp(a, 0, 255);
    if (a <= 0) return;
    float cr = r / 255.0f, cg = g / 255.0f, cb = b / 255.0f;
    g_scratch.clear();
    g_scratch.reserve(54);
    push9(g_scratch, x0, y0, 0, 0, 0, 1, cr, cg, cb);
    push9(g_scratch, x1, y0, 0, 0, 0, 1, cr, cg, cb);
    push9(g_scratch, x1, y1, 0, 0, 0, 1, cr, cg, cb);
    push9(g_scratch, x0, y0, 0, 0, 0, 1, cr, cg, cb);
    push9(g_scratch, x1, y1, 0, 0, 0, 1, cr, cg, cb);
    push9(g_scratch, x0, y1, 0, 0, 0, 1, cr, cg, cb);
    K.P.BindBuffer(kArray, K.vboLine);
    K.P.BufferData(kArray, (std::intptr_t)(g_scratch.size() * sizeof(float)), g_scratch.data(), kDynamic);
    K.lit = 0; K.vert = 1; K.two = 0;
    K.alpha = a / 255.0f;
    K.ndcX = K.ndcY = 0;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float id[16];
    ident(id);
    draw_mesh(K.vboLine, 6, id, id, GL_TRIANGLES);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    K.lit = 1; K.vert = 0; K.alpha = 1;
}

static void line33(V3 a, V3 b, int r, int g, int bl, int alpha) {
    alpha = std::clamp(alpha, 0, 255);
    if (g_lineA >= 0 && g_lineA != alpha) flush_lines33();
    g_lineA = alpha;
    float cr = r / 255.0f, cg = g / 255.0f, cb = bl / 255.0f;
    float v[18] = {
        a.x, a.y, a.z, 0, 0, 1, cr, cg, cb,
        b.x, b.y, b.z, 0, 0, 1, cr, cg, cb,
    };
    g_lines.insert(g_lines.end(), v, v + 18);
}

static void outline33(int r, int og, int ob) {
    flush_lines33();
    if (S.selected.empty()) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    K.lit = 0; K.vert = 0; K.alpha = 1; K.ndcX = K.ndcY = 0;
    K.color[0] = K.color[1] = K.color[2] = 0;
    for (int s : S.selected) if (active_slot(s)) shape33(g.entities[s]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    K.color[0] = r / 255.0f; K.color[1] = og / 255.0f; K.color[2] = ob / 255.0f;
    if (K.wideLines) {
        glLineWidth(4.0f);
        for (int s : S.selected) if (active_slot(s)) shape33(g.entities[s]);
    } else {
        glLineWidth(1.0f);
        float dx = 3.0f / (float)std::max(S.w, 1);
        float dy = 3.0f / (float)std::max(S.h, 1);
        const float off[5][2] = { {0, 0}, {dx, dy}, {dx, -dy}, {-dx, dy}, {-dx, -dy} };
        for (int i = 0; i < 5; ++i) {
            K.ndcX = off[i][0]; K.ndcY = off[i][1];
            for (int s : S.selected) if (active_slot(s)) shape33(g.entities[s]);
        }
        K.ndcX = K.ndcY = 0;
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.0f);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    K.lit = 1;
}

static const char* kPartVs = R"GLSL(
#version 330
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform mat4 uView;
uniform mat4 uProj;
uniform float uFogDist;
out vec2 vUv;
out vec4 vColor;
out float vFog;
void main() {
    vec4 eye = uView * vec4(aPos, 1.0);
    gl_Position = uProj * eye;
    vUv = aUv;
    vColor = aColor;
    vFog = 0.0;
    if (uFogDist > 0.001) {
        float dist = length(eye.xyz);
        float start = uFogDist * 0.25;
        vFog = clamp((dist - start) / max(uFogDist - start, 0.001), 0.0, 1.0);
    }
}
)GLSL";

static const char* kPartFs = R"GLSL(
#version 330
in vec2 vUv;
in vec4 vColor;
in float vFog;
uniform sampler2D uTex;
uniform vec3 uFogColor;
out vec4 frag;
void main() {
    vec4 t = texture(uTex, vUv);
    vec3 col = mix(t.rgb * vColor.rgb, uFogColor, vFog);
    frag = vec4(col, t.a * vColor.a);
}
)GLSL";

static bool part_program() {
    if (K.partProg) return true;
    GLuint vs = compile_one(0x8B31, kPartVs);
    GLuint fs = compile_one(0x8B30, kPartFs);
    if (!vs || !fs) {
        if (vs) K.P.DeleteShader(vs);
        if (fs) K.P.DeleteShader(fs);
        return false;
    }
    GLuint prog = K.P.CreateProgram();
    K.P.Attach(prog, vs);
    K.P.Attach(prog, fs);
    K.P.Link(prog);
    K.P.DeleteShader(vs);
    K.P.DeleteShader(fs);
    GLint ok = 0;
    K.P.GetProgramiv(prog, 0x8B82, &ok);
    if (!ok) return false;
    K.partProg = prog;
    K.partUView = K.P.GetLoc(prog, "uView");
    K.partUProj = K.P.GetLoc(prog, "uProj");
    K.partUTex = K.P.GetLoc(prog, "uTex");
    K.partUFog = K.P.GetLoc(prog, "uFogColor");
    K.partUFogDist = K.P.GetLoc(prog, "uFogDist");
    K.P.GenVao(1, &K.partVao);
    K.P.GenBuffers(1, &K.partVbo);
    K.P.BindVao(K.partVao);
    K.P.BindBuffer(kArray, K.partVbo);
    K.P.EnableAttrib(0);
    K.P.EnableAttrib(1);
    K.P.EnableAttrib(2);
    K.P.AttribPtr(0, 3, 0x1406, 0, 9 * (GLsizei)sizeof(float), (void*)0);
    K.P.AttribPtr(1, 2, 0x1406, 0, 9 * (GLsizei)sizeof(float), (void*)(3 * sizeof(float)));
    K.P.AttribPtr(2, 4, 0x1406, 0, 9 * (GLsizei)sizeof(float), (void*)(5 * sizeof(float)));
    K.P.BindVao(0);
    return true;
}

static void particles33(const PartDraw* draws, int n) {
    if (!draws || n <= 0 || !part_program()) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    K.P.Use(K.partProg);
    K.P.UniMat4(K.partUView, 1, 0, K.cam);
    K.P.UniMat4(K.partUProj, 1, 0, K.proj);
    if (K.partUFog >= 0) {
        float col[3] = { g_fogR / 255.0f, g_fogG / 255.0f, g_fogB / 255.0f };
        K.P.Uni3fv(K.partUFog, 1, col);
    }
    if (K.partUFogDist >= 0) K.P.Uni1f(K.partUFogDist, (g_fog && g_fogDist > 0) ? g_fogDist * kUnit : 0.f);
    if (K.P.ActiveTex) K.P.ActiveTex(0x84C0);
    K.P.Uni1i(K.partUTex, 0);
    K.P.BindVao(K.partVao);
    std::vector<float> v;
    int i = 0;
    while (i < n) {
        int image = draws[i].image;
        v.clear();
        while (i < n && draws[i].image == image) {
            const PartDraw& d = draws[i++];
            V3 p0, p1, p2, p3;
            part_billboard(d, p0, p1, p2, p3);
            const V3 corner[6] = { p0, p1, p2, p0, p2, p3 };
            const float uv[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
            for (int k = 0; k < 6; ++k) {
                v.push_back(corner[k].x); v.push_back(corner[k].y); v.push_back(corner[k].z);
                v.push_back(uv[k * 2]); v.push_back(uv[k * 2 + 1]);
                v.push_back(d.r / 255.f); v.push_back(d.g / 255.f);
                v.push_back(d.b / 255.f); v.push_back(d.a / 255.f);
            }
        }
        GLuint tex = part_gl_tex(image);
        if (!tex || v.empty()) continue;
        glBindTexture(GL_TEXTURE_2D, tex);
        K.P.BindBuffer(kArray, K.partVbo);
        K.P.BufferData(kArray, (std::intptr_t)(v.size() * sizeof(float)), v.data(), kDynamic);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 9));
    }
    K.P.BindVao(0);
    K.P.Use(K.prog);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

#include "render3d_vk.inl"
