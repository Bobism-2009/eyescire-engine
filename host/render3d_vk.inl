/* Vulkan 1.4 draws the same picture as the OpenGL 3.3 path. */

static float g_vkProj[16], g_vkCam[16], g_vkLightVP[16];
static float g_vkAmb[4] = { 0.55f, 0.55f, 0.55f, 0 };
static int g_vkRW = 1, g_vkRH = 1;
static std::vector<float> g_vkLines;
static int g_vkLineA = -1;
static std::vector<float> g_vkBox, g_vkSphere, g_vkCyl, g_vkSky;
static int g_vkUnits = 0;
static std::unordered_map<const Model*, std::vector<float>> g_vkModel9, g_vkModel12;

struct VkKeepKey { const void* p; int n; int stride; };
struct VkKeepHash {
    size_t operator()(const VkKeepKey& k) const {
        return ((size_t)k.p >> 4) ^ (size_t)k.n * 131u ^ (size_t)k.stride;
    }
};
struct VkKeepEq {
    bool operator()(const VkKeepKey& a, const VkKeepKey& b) const {
        return a.p == b.p && a.n == b.n && a.stride == b.stride;
    }
};
static std::unordered_map<VkKeepKey, uint64_t, VkKeepHash, VkKeepEq> g_vkKeep;
static int g_vkKeepEpoch = -1;

static int vk_mesh_stable(const float* p) {
    if (!p) return 0;
    if (p == g_vkBox.data() || p == g_vkSphere.data() || p == g_vkCyl.data() || p == g_vkSky.data()) return 1;
    for (const auto& kv : g_vkModel9) if (p == kv.second.data()) return 1;
    for (const auto& kv : g_vkModel12) if (p == kv.second.data()) return 1;
    return 0;
}

static uint64_t vk_kept(const float* p, int n, int stride) {
    int ep = nexa_vk14::epoch();
    if (ep != g_vkKeepEpoch) { g_vkKeep.clear(); g_vkKeepEpoch = ep; }
    if (!vk_mesh_stable(p) || n <= 0) return ~0ull;
    VkKeepKey key{ p, n, stride };
    auto it = g_vkKeep.find(key);
    if (it != g_vkKeep.end()) return it->second;
    uint64_t off = nexa_vk14::vert_keep(p, (uint64_t)n * (uint64_t)stride);
    if (off == ~0ull) return off;
    g_vkKeep.emplace(key, off);
    return off;
}
static std::unordered_map<int, std::vector<unsigned char>> g_vkPartFlip;

static void vk_units() {
    if (g_vkUnits) return;
    g_vkUnits = 1;
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
        for (int i = 0; i < GRID; ++i) for (int j = 0; j < GRID; ++j) {
            float u0 = (float)i / GRID, u1 = (float)(i + 1) / GRID;
            float v0 = (float)j / GRID, v1 = (float)(j + 1) / GRID;
            float q[4][9];
            corner(u0, v0, q[0]); corner(u1, v0, q[1]); corner(u1, v1, q[2]); corner(u0, v1, q[3]);
            push_quad(g_vkBox, q[0], q[1], q[2], q[3]);
        }
    }
    const int SEG = 40, RING = 24;
    for (int j = 0; j < RING; ++j) {
        std::vector<float> strip;
        for (int i = 0; i <= SEG; ++i) for (int k = 0; k < 2; ++k) {
            float th = (float)(j + k) / RING * 3.14159265f;
            float ph = (float)i / SEG * 6.2831853f;
            float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
            push9(strip, x * .5f, y * .5f, z * .5f, x, y, z, 1, 1, 1);
        }
        push_strip(g_vkSphere, strip);
    }
    const int CS = 48;
    std::vector<float> side;
    for (int i = 0; i <= CS; ++i) {
        float a = (float)i / CS * 6.2831853f;
        float x = std::cos(a), z = std::sin(a);
        push9(side, x * .5f, .5f, z * .5f, x, 0, z, 1, 1, 1);
        push9(side, x * .5f, -.5f, z * .5f, x, 0, z, 1, 1, 1);
    }
    push_strip(g_vkCyl, side);
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap == 0 ? .5f : -.5f, ny = cap == 0 ? 1.f : -1.f;
        std::vector<float> fan;
        push9(fan, 0, y, 0, 0, ny, 0, 1, 1, 1);
        for (int i = 0; i <= CS; ++i) {
            float a = (float)i / CS * 6.2831853f;
            push9(fan, std::cos(a) * .5f, y, std::sin(a) * .5f, 0, ny, 0, 1, 1, 1);
        }
        push_fan(g_vkCyl, fan);
    }
    static const float cube[] = {
        1,-1,-1,  1, 1,-1,  1, 1, 1,   1,-1,-1,  1, 1, 1,  1,-1, 1,
       -1,-1, 1, -1, 1, 1, -1, 1,-1,  -1,-1, 1, -1, 1,-1, -1,-1,-1,
       -1, 1,-1, -1, 1, 1,  1, 1, 1,  -1, 1,-1,  1, 1, 1,  1, 1,-1,
       -1,-1, 1, -1,-1,-1,  1,-1,-1,  -1,-1, 1,  1,-1,-1,  1,-1, 1,
       -1,-1, 1,  1,-1, 1,  1, 1, 1,  -1,-1, 1,  1, 1, 1, -1, 1, 1,
        1,-1,-1, -1,-1,-1, -1, 1,-1,   1,-1,-1, -1, 1,-1,  1, 1,-1,
    };
    g_vkSky.assign(cube, cube + 36 * 3);
}

static const std::vector<float>& vk_model9(const Model& m) {
    auto it = g_vkModel9.find(&m);
    if (it != g_vkModel9.end()) return it->second;
    std::vector<float> v;
    for (size_t k = 0; k + 2 < m.tri.size(); k += 3) {
        V3 a = m.pos[m.tri[k]], b = m.pos[m.tri[k + 1]], c = m.pos[m.tri[k + 2]];
        V3 nm = norm(cross(b - a, c - a));
        push9(v, a.x, a.y, a.z, nm.x, nm.y, nm.z, 1, 1, 1);
        push9(v, b.x, b.y, b.z, nm.x, nm.y, nm.z, 1, 1, 1);
        push9(v, c.x, c.y, c.z, nm.x, nm.y, nm.z, 1, 1, 1);
    }
    return g_vkModel9[&m] = std::move(v);
}

static const std::vector<float>& vk_model12(const Model& m) {
    auto it = g_vkModel12.find(&m);
    if (it != g_vkModel12.end()) return it->second;
    std::vector<float> v;
    for (size_t k = 0; k + 2 < m.tri.size(); k += 3) {
        V3 p[3], nm[3];
        float u[3], vv[3];
        for (int c = 0; c < 3; ++c) {
            p[c] = m.pos[m.tri[k + c]];
            nm[c] = (k + c < m.cn.size() && dot(m.cn[k + c], m.cn[k + c]) > 1e-8f) ? norm(m.cn[k + c]) : v3(0, 0, 0);
            u[c] = (k + c) * 2 + 1 < m.cuv.size() ? m.cuv[(k + c) * 2] : 0;
            vv[c] = (k + c) * 2 + 1 < m.cuv.size() ? m.cuv[(k + c) * 2 + 1] : 0;
        }
        V3 face = norm(cross(p[1] - p[0], p[2] - p[0]));
        V3 e1 = p[1] - p[0], e2 = p[2] - p[0];
        float du1 = u[1] - u[0], dv1 = vv[1] - vv[0], du2 = u[2] - u[0], dv2 = vv[2] - vv[0];
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
            float q[12] = { p[c].x, p[c].y, p[c].z, nrm.x, nrm.y, nrm.z, t.x, t.y, t.z, hand, u[c], vv[c] };
            v.insert(v.end(), q, q + 12);
        }
    }
    return g_vkModel12[&m] = std::move(v);
}

struct VkLocal {
    float shape[16];
    const float* v = nullptr;
    int n = 0;
    float two = 0;
    const Model* mapped = nullptr;
};

static VkLocal vk_local(const Entity& e) {
    VkLocal L{};
    vk_units();
    float w = (float)e.w, h = (float)e.h, d = (float)depth_of(e);
    if (w <= 0 || h <= 0) return L;
    float sx, sy, sz;
    const float* src = g_vkBox.data();
    int n = (int)(g_vkBox.size() / 9);
    if (e.kind == "sphere") {
        float dm = std::min(w, h) * kUnit;
        sx = sy = sz = dm; src = g_vkSphere.data(); n = (int)(g_vkSphere.size() / 9);
    } else if (e.kind == "cylinder") {
        sx = w * kUnit; sy = h * kUnit; sz = w * kUnit;
        src = g_vkCyl.data(); n = (int)(g_vkCyl.size() / 9);
    } else if (e.kind == "capsule") {
        sx = w * kUnit; sy = h * kUnit; sz = w * kUnit;
        static std::vector<float> cap;
        cap.clear();
        CapsuleLocal cl = capsule_local(w, h);
        capsule_emit(cl, sx, sy, sz, [&](float px, float py, float pz, float nx, float ny, float nz) {
            push9(cap, px, py, pz, nx, ny, nz, 1, 1, 1);
        });
        src = cap.data(); n = (int)(cap.size() / 9);
    } else {
        const Model* m = nullptr;
        if (e.kind == "model" && !e.model.empty()) m = &model_load(model_path(e.model));
        if (m && m->ok) {
            float s = std::max(w, h) * kUnit;
            sx = sy = sz = s;
            const std::vector<float>& mv = vk_model9(*m);
            src = mv.data(); n = (int)(mv.size() / 9); L.two = 1;
            if (model_has_maps(*m)) L.mapped = m;
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
    mul(L.shape, tr, sc);
    L.v = src; L.n = n;
    return L;
}

static void vk_pack(float* o, const float* mvp, const float* mv, const float* light, const float* n9,
                    float cr, float cg, float cb, float ca, float lit, float vert, float two,
                    float ndcX, float ndcY, int maps, const float amb[4]) {
    std::memcpy(o, mvp, 64);
    std::memcpy(o + 16, mv, 64);
    if (light) std::memcpy(o + 32, light, 64);
    else { float id[16]; ident(id); std::memcpy(o + 32, id, 64); }
    std::memset(o + 48, 0, 64);
    if (n9) {
        o[48] = n9[0]; o[49] = n9[1]; o[50] = n9[2];
        o[52] = n9[3]; o[53] = n9[4]; o[54] = n9[5];
        o[56] = n9[6]; o[57] = n9[7]; o[58] = n9[8];
    }
    o[63] = 1;
    o[64] = cr; o[65] = cg; o[66] = cb; o[67] = ca;
    o[68] = lit; o[69] = vert; o[70] = two; o[71] = ndcX;
    o[72] = ndcY; o[73] = 0; o[74] = (float)maps; o[75] = 0;
    std::memcpy(o + 76, amb, 16);
}

static void vk_draw9(const float* v, int n, const float* mvp, const float* mv, int shadow,
                     float lit, float vert, float two, float alpha, float cr, float cg, float cb,
                     float ndcX, float ndcY, int kind) {
    if (!v || n <= 0) return;
    float nm[9], light[16], draw[80];
    normal_matrix(mv, nm);
    if (shadow) { /* light mvp already in g_vkLightVP * shape, passed via mv's pair */ }
    ident(light);
    vk_pack(draw, mvp, mv, light, nm, cr, cg, cb, alpha, lit, vert, two, ndcX, ndcY, 0, g_vkAmb);
    if (shadow) {
        /* replaced below when the caller passes light */
    }
    nexa_vk14::mesh_draw(v, n, draw, kind);
}

static void vk_draw_world(const float* v, int n, const float* shape, int useShadow,
                          float lit, float vert, float two, float alpha, float cr, float cg, float cb,
                          float ndcX, float ndcY, int kind, const float* amb) {
    if (!v || n <= 0) return;
    float mv[16], mvp[16], light[16], nm[9], draw[80];
    mul(mv, g_vkCam, shape);
    mul(mvp, g_vkProj, mv);
    if (useShadow) mul(light, g_vkLightVP, shape);
    else ident(light);
    normal_matrix(mv, nm);
    vk_pack(draw, mvp, mv, light, nm, cr, cg, cb, alpha, lit, vert, two, ndcX, ndcY, 0, amb ? amb : g_vkAmb);
    uint64_t off = vk_kept(v, n, 36);
    if (off != ~0ull) nexa_vk14::mesh_draw_kept(off, n, draw, kind);
    else nexa_vk14::mesh_draw(v, n, draw, kind);
}

static void vk_flush_lines() {
    if (g_vkLines.empty()) return;
    float mvp[16], mv[16];
    mul(mvp, g_vkProj, g_vkCam);
    std::memcpy(mv, g_vkCam, sizeof(mv));
    float a = std::clamp(g_vkLineA, 0, 255) / 255.0f;
    vk_draw9(g_vkLines.data(), (int)(g_vkLines.size() / 9), mvp, mv, 0, 0, 1, 0, a, 1, 1, 1, 0, 0, 7);
    g_vkLines.clear();
    g_vkLineA = -1;
}

static int vk_kind(float two, float alpha) {
    if (alpha < 0.999f) return two > 0.5f ? 3 : 2;
    return two > 0.5f ? 1 : 0;
}

static void vk_entity_draw(const Entity& e, float ndcX, float ndcY, int forceKind, const float* amb) {
    VkLocal L = vk_local(e);
    if (!L.v || L.n <= 0) return;
    float alpha = std::clamp(e.a, 0, 255) / 255.0f;
    float cr = e.r / 255.0f, cg = e.g / 255.0f, cb = e.b / 255.0f;
    int kind = forceKind >= 0 ? forceKind : vk_kind(L.two, alpha);
    float lit = forceKind >= 0 ? 0.f : 1.f;
    if (L.mapped && forceKind < 0 && lit > 0.5f) {
        const std::vector<float>& mv = vk_model12(*L.mapped);
        if (!mv.empty()) {
            float model[16], camv[16], mvp[16], light[16], nm[9], draw[80];
            mul(camv, g_vkCam, L.shape);
            mul(mvp, g_vkProj, camv);
            if (g_shadows) mul(light, g_vkLightVP, L.shape);
            else ident(light);
            normal_matrix(camv, nm);
            vk_pack(draw, mvp, camv, light, nm, cr, cg, cb, alpha, 1, 0, 1, ndcX, ndcY, model_map_bits(*L.mapped), amb ? amb : g_vkAmb);
            nexa_vk14::Pix maps[5] = {
                { L.mapped->base.rgb.empty() ? nullptr : L.mapped->base.rgb.data(), L.mapped->base.w, L.mapped->base.h },
                { L.mapped->nrm.rgb.empty() ? nullptr : L.mapped->nrm.rgb.data(), L.mapped->nrm.w, L.mapped->nrm.h },
                { L.mapped->metal.rgb.empty() ? nullptr : L.mapped->metal.rgb.data(), L.mapped->metal.w, L.mapped->metal.h },
                { L.mapped->rough.rgb.empty() ? nullptr : L.mapped->rough.rgb.data(), L.mapped->rough.w, L.mapped->rough.h },
                { L.mapped->height.rgb.empty() ? nullptr : L.mapped->height.rgb.data(), L.mapped->height.w, L.mapped->height.h },
            };
            int nv = (int)(mv.size() / 12);
            uint64_t off = vk_kept(mv.data(), nv, 48);
            if (off != ~0ull) nexa_vk14::mat_draw_kept(off, nv, draw, alpha < 0.999f ? 1 : 0, maps);
            else nexa_vk14::mat_draw(mv.data(), nv, draw, alpha < 0.999f ? 1 : 0, maps);
            return;
        }
    }
    vk_draw_world(L.v, L.n, L.shape, g_shadows && forceKind < 0, lit, 0, L.two, alpha, cr, cg, cb, ndcX, ndcY, kind, amb);
}

static void vk_frame_entity(const Entity& e, int slot) {
    vk_flush_lines();
    bool held = slot == g_viewSlot;
    float amb[4] = { g_vkAmb[0], g_vkAmb[1], g_vkAmb[2], g_vkAmb[3] };
    if (held) {
        amb[0] = std::min(1.f, amb[0] + 0.28f);
        amb[1] = std::min(1.f, amb[1] + 0.24f);
        amb[2] = std::min(1.f, amb[2] + 0.18f);
        nexa_vk14::depth_range(0.f, 0.18f);
    }
    vk_entity_draw(e, 0, 0, -1, amb);
    if (held) nexa_vk14::depth_range(0.f, 1.f);
    if (e.selected) S.selected.push_back(slot);
}

static void vk_frame_line(V3 a, V3 b, int r, int g, int bl, int alpha) {
    alpha = std::clamp(alpha, 0, 255);
    if (g_vkLineA >= 0 && g_vkLineA != alpha) vk_flush_lines();
    g_vkLineA = alpha;
    float cr = r / 255.0f, cg = g / 255.0f, cb = bl / 255.0f;
    float v[18] = { a.x, a.y, a.z, 0, 0, 1, cr, cg, cb, b.x, b.y, b.z, 0, 0, 1, cr, cg, cb };
    g_vkLines.insert(g_vkLines.end(), v, v + 18);
}

static void vk_frame_cover(const V3* pts, int ntri, int r, int g, int b, int alpha) {
    if (!pts || ntri <= 0) return;
    vk_flush_lines();
    std::vector<float> v;
    float cr = r / 255.f, cg = g / 255.f, cb = b / 255.f;
    for (int i = 0; i < ntri; ++i) {
        V3 n = norm(cross(pts[i * 3 + 1] - pts[i * 3], pts[i * 3 + 2] - pts[i * 3]));
        push9(v, pts[i * 3].x, pts[i * 3].y, pts[i * 3].z, n.x, n.y, n.z, cr, cg, cb);
        push9(v, pts[i * 3 + 1].x, pts[i * 3 + 1].y, pts[i * 3 + 1].z, n.x, n.y, n.z, cr, cg, cb);
        push9(v, pts[i * 3 + 2].x, pts[i * 3 + 2].y, pts[i * 3 + 2].z, n.x, n.y, n.z, cr, cg, cb);
    }
    float id[16]; ident(id);
    float mvp[16]; mul(mvp, g_vkProj, g_vkCam);
    vk_draw9(v.data(), (int)(v.size() / 9), mvp, g_vkCam, 0, 0, 1, 0, std::clamp(alpha, 0, 255) / 255.f, 1, 1, 1, 0, 0, 4);
    (void)id;
}

static void vk_frame_cover_model(const Entity& e, int r, int g, int b, int alpha) {
    vk_flush_lines();
    Entity coat = e;
    coat.r = r; coat.g = g; coat.b = b; coat.a = alpha;
    vk_entity_draw(coat, 0, 0, 4, nullptr);
}

static void vk_frame_painted(const float* v, int ntri) {
    if (!v || ntri <= 0) return;
    vk_flush_lines();
    std::vector<float> o;
    for (int i = 0; i < ntri * 3; ++i) {
        const float* p = v + i * 6;
        push9(o, p[0], p[1], p[2], 0, 0, 1, p[3], p[4], p[5]);
    }
    float mvp[16]; mul(mvp, g_vkProj, g_vkCam);
    vk_draw9(o.data(), (int)(o.size() / 9), mvp, g_vkCam, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 5);
}

static void vk_frame_sector(const V3* pts, int ntri, int r, int g, int b, int a) {
    if (!pts || ntri <= 0) return;
    vk_flush_lines();
    std::vector<float> o;
    float cr = r / 255.f, cg = g / 255.f, cb = b / 255.f;
    for (int i = 0; i < ntri * 3; ++i) push9(o, pts[i].x, pts[i].y, pts[i].z, 0, 0, 1, cr, cg, cb);
    float mvp[16]; mul(mvp, g_vkProj, g_vkCam);
    vk_draw9(o.data(), (int)(o.size() / 9), mvp, g_vkCam, 0, 0, 1, 0, std::clamp(a, 0, 255) / 255.f, 1, 1, 1, 0, 0, 6);
}

static void vk_frame_screen(float x0, float y0, float x1, float y1, int r, int g, int b, int a) {
    vk_flush_lines();
    a = std::clamp(a, 0, 255);
    if (a <= 0) return;
    y0 = -y0; y1 = -y1;
    float cr = r / 255.f, cg = g / 255.f, cb = b / 255.f;
    std::vector<float> o;
    push9(o, x0, y0, 0, 0, 0, 1, cr, cg, cb);
    push9(o, x1, y0, 0, 0, 0, 1, cr, cg, cb);
    push9(o, x1, y1, 0, 0, 0, 1, cr, cg, cb);
    push9(o, x0, y0, 0, 0, 0, 1, cr, cg, cb);
    push9(o, x1, y1, 0, 0, 0, 1, cr, cg, cb);
    push9(o, x0, y1, 0, 0, 0, 1, cr, cg, cb);
    float id[16]; ident(id);
    vk_draw9(o.data(), 6, id, id, 0, 0, 1, 0, a / 255.f, 1, 1, 1, 0, 0, 6);
}

static void vk_frame_outline(int r, int og, int b) {
    vk_flush_lines();
    if (S.selected.empty()) return;
    for (int s : S.selected) if (active_slot(s)) vk_entity_draw(g.entities[s], 0, 0, 8, nullptr);
    float dx = 3.0f / (float)std::max(g_vkRW, 1);
    float dy = 3.0f / (float)std::max(g_vkRH, 1);
    const float off[5][2] = { {0, 0}, {dx, dy}, {dx, -dy}, {-dx, dy}, {-dx, -dy} };
    for (int i = 0; i < 5; ++i) {
        for (int s : S.selected) if (active_slot(s)) {
            Entity e = g.entities[s];
            e.r = r; e.g = og; e.b = b; e.a = 255;
            vk_entity_draw(e, off[i][0], off[i][1], 9, nullptr);
        }
    }
}

static void vk_frame_particles() {
    vk_flush_lines();
    PartDraw buf[kPartMax];
    int n = part_collect_3d(buf, kPartMax);
    if (n <= 0) return;
    float cam[36];
    std::memcpy(cam, g_vkCam, 64);
    std::memcpy(cam + 16, g_vkProj, 64);
    cam[32] = g_fogR / 255.f; cam[33] = g_fogG / 255.f; cam[34] = g_fogB / 255.f;
    cam[35] = (g_fog && g_fogDist > 0) ? g_fogDist * kUnit : 0.f;
    int i = 0;
    while (i < n) {
        int image = buf[i].image;
        std::vector<float> v;
        while (i < n && buf[i].image == image) {
            const PartDraw& d = buf[i++];
            float cx = d.x * kUnit, cy = -d.y * kUnit, cz = -d.z * kUnit, hs = d.size * kUnit * 0.5f;
            V3 rgt = C.right * hs, up = C.up * hs, c = v3(cx, cy, cz);
            V3 p0 = c - rgt - up, p1 = c + rgt - up, p2 = c + rgt + up, p3 = c - rgt + up;
            const V3 corner[6] = { p0, p1, p2, p0, p2, p3 };
            const float uv[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
            for (int k = 0; k < 6; ++k) {
                v.push_back(corner[k].x); v.push_back(corner[k].y); v.push_back(corner[k].z);
                v.push_back(uv[k * 2]); v.push_back(uv[k * 2 + 1]);
                v.push_back(d.r / 255.f); v.push_back(d.g / 255.f); v.push_back(d.b / 255.f); v.push_back(d.a / 255.f);
            }
        }
        const PartPix* pix = part_pixels(image);
        if (!pix || pix->w < 1 || v.empty()) continue;
        auto fit = g_vkPartFlip.find(image);
        if (fit == g_vkPartFlip.end()) {
            std::vector<unsigned char> flip((size_t)pix->w * pix->h * 4);
            for (int y = 0; y < pix->h; ++y)
                std::memcpy(flip.data() + (size_t)(pix->h - 1 - y) * pix->w * 4,
                            pix->rgba.data() + (size_t)y * pix->w * 4, (size_t)pix->w * 4);
            fit = g_vkPartFlip.emplace(image, std::move(flip)).first;
        }
        nexa_vk14::part_draw(v.data(), (int)(v.size() / 9), fit->second.data(), pix->w, pix->h, cam);
    }
}

static int vk_frame_begin(int w, int h, double fov_deg, int br, int bg, int bb) {
    place(C.sx, C.sy, w, h);
    int rw = w, rh = h;
    if (g_hd && w < 1920 && h < 1080) {
        rw = w * 2; rh = h * 2;
        if (rw > 4096) rw = 4096;
        if (rh > 4096) rh = 4096;
    } else if (w > 1920 || h > 1080) {
        double sx = 1920.0 / (double)w, sy = 1080.0 / (double)h;
        double s = sx < sy ? sx : sy;
        rw = (int)(w * s); rh = (int)(h * s);
        if (rw < 1) rw = 1;
        if (rh < 1) rh = 1;
    }
    g_vkRW = rw; g_vkRH = rh;
    S.w = rw; S.h = rh;
    S.selected.clear();
    g_vkLines.clear();
    g_vkLineA = -1;
    nexa_vk14::set_vsync(g_vsync);
    int samples = aa_samples();
    if (samples < 2 && g_aa) samples = 4;
    if (!nexa_vk14::scene_begin(w, h, rw, rh, samples, br / 255.f, bg / 255.f, bb / 255.f)) {
        if (rw == w && rh == h) return 0;
        rw = w; rh = h;
        g_vkRW = rw; g_vkRH = rh;
        S.w = rw; S.h = rh;
        if (!nexa_vk14::scene_begin(w, h, rw, rh, samples, br / 255.f, bg / 255.f, bb / 255.f)) return 0;
    }

    const double zn = kNear, zf = 4000.0;
    const double top = zn * std::tan(fov_deg * 0.5 * 3.14159265358979 / 180.0);
    const double right = top * (double)rw / (double)rh;
    const double rl = right - (-right), tb = top - (-top), fn = zf - zn;
    std::memset(g_vkProj, 0, sizeof(g_vkProj));
    g_vkProj[0] = (float)(2 * zn / rl);
    g_vkProj[5] = (float)(2 * zn / tb);
    g_vkProj[10] = (float)(-(zf + zn) / fn);
    g_vkProj[11] = -1;
    g_vkProj[14] = (float)(-2 * zf * zn / fn);

    float basis[16] = {
        C.right.x, C.up.x, -C.fwd.x, 0,
        C.right.y, C.up.y, -C.fwd.y, 0,
        C.right.z, C.up.z, -C.fwd.z, 0,
        0, 0, 0, 1,
    };
    vk_units();
    const SkyCpu* sky = sky_cpu();
    if (sky && sky->face[0].w > 0 && !sky->face[0].rgb.empty()) {
        float view[16];
        mul(view, g_vkProj, basis);
        float pc[20];
        std::memcpy(pc, view, 64);
        pc[16] = g_fogR / 255.f; pc[17] = g_fogG / 255.f; pc[18] = g_fogB / 255.f; pc[19] = g_fog ? 1.f : 0.f;
        nexa_vk14::Pix faces[6];
        int ok = 1;
        for (int i = 0; i < 6; ++i) {
            if (sky->face[i].w < 1 || sky->face[i].rgb.empty()) { ok = 0; break; }
            faces[i] = { sky->face[i].rgb.data(), sky->face[i].w, sky->face[i].h };
        }
        if (ok) {
            uint64_t off = vk_kept(g_vkSky.data(), 36, 12);
            if (off != ~0ull) nexa_vk14::sky_draw_kept(off, 36, pc, faces);
            else nexa_vk14::sky_draw(g_vkSky.data(), 36, pc, faces);
        }
    } else {
        int cr = br, cg = bg, cb = bb;
        if (g_fog) { cr = g_fogR; cg = g_fogG; cb = g_fogB; }
        int r0 = g_fog ? cr : std::min(255, cr + 14);
        int g0 = g_fog ? cg : std::min(255, cg + 16);
        int b0 = g_fog ? cb : std::min(255, cb + 22);
        float lr = r0 / 255.f, lg = g0 / 255.f, lb = b0 / 255.f;
        float dr = cr / 255.f, dg = cg / 255.f, db = cb / 255.f;
        std::vector<float> bgv;
        auto put = [&](float x, float y, float rr, float gg, float bbv) {
            push9(bgv, x, y, 0, 0, 0, 1, rr, gg, bbv);
        };
        put(-1, 1, lr, lg, lb); put(1, 1, lr, lg, lb); put(1, -1, dr, dg, db);
        put(-1, 1, lr, lg, lb); put(1, -1, dr, dg, db); put(-1, -1, dr, dg, db);
        float id[16]; ident(id);
        vk_draw9(bgv.data(), 6, id, id, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 5);
    }

    float tr[16];
    ident(tr);
    tr[12] = -C.eye.x; tr[13] = -C.eye.y; tr[14] = -C.eye.z;
    mul(g_vkCam, basis, tr);

    float lpos[64] = {}, ldif[64] = {}, lspot[64] = {}, lsp[64] = {};
    for (int i = 0; i < 16; ++i) {
        lpos[i * 4 + 2] = 1;
        lspot[i * 4 + 2] = -1;
        lsp[i * 4] = 2;
    }
    g_vkAmb[0] = g_vkAmb[1] = g_vkAmb[2] = 0.55f; g_vkAmb[3] = 0;
    bool any_ambient = false, any_sun = false, has_lamp = false;
    for (const Light& l : g_lights) {
        if (l.type == 0) any_sun = true;
        if (l.type == 1 || l.type == 2) has_lamp = true;
    }
    int n = 0, sunSlot = -1;
    V3 shine = kDefaultLight * -1.f;
    const float noSpot[4] = { 0, 0, -1, 0 };
    auto put_dir = [&](int slot, float x, float y, float z, float r, float g, float b) {
        float eye[4];
        xform(g_vkCam, x, y, z, 0, eye);
        lpos[slot * 4] = eye[0]; lpos[slot * 4 + 1] = eye[1]; lpos[slot * 4 + 2] = eye[2]; lpos[slot * 4 + 3] = 0;
        ldif[slot * 4] = r; ldif[slot * 4 + 1] = g; ldif[slot * 4 + 2] = b;
        lspot[slot * 4] = noSpot[0]; lspot[slot * 4 + 1] = noSpot[1]; lspot[slot * 4 + 2] = noSpot[2];
        lsp[slot * 4] = 2;
    };
    if (!any_sun && !has_lamp) {
        put_dir(n++, kDefaultLight.x, kDefaultLight.y, kDefaultLight.z, 0.95f, 0.95f, 0.95f);
        sunSlot = n - 1;
    }
    for (const Light& l : g_lights) {
        if (l.type == 3) {
            if (!any_ambient) { g_vkAmb[0] = g_vkAmb[1] = g_vkAmb[2] = 0; any_ambient = true; }
            g_vkAmb[0] += l.r; g_vkAmb[1] += l.g; g_vkAmb[2] += l.b;
            continue;
        }
        if (n >= 16) continue;
        if (l.type == 0) {
            int slot = n;
            put_dir(n++, -l.dir.x, -l.dir.y, -l.dir.z, l.r, l.g, l.b);
            if (sunSlot < 0) { sunSlot = slot; shine = l.dir; }
        } else {
            float eye[4], sd[4];
            xform(g_vkCam, l.pos.x, l.pos.y, l.pos.z, 1, eye);
            xform(g_vkCam, l.dir.x, l.dir.y, l.dir.z, 0, sd);
            lpos[n * 4] = eye[0]; lpos[n * 4 + 1] = eye[1]; lpos[n * 4 + 2] = eye[2]; lpos[n * 4 + 3] = 1;
            ldif[n * 4] = l.r; ldif[n * 4 + 1] = l.g; ldif[n * 4 + 2] = l.b;
            lspot[n * 4] = sd[0]; lspot[n * 4 + 1] = sd[1]; lspot[n * 4 + 2] = sd[2];
            float cutoff = 2, expo = 0, q = 1.0f / (l.range * l.range);
            if (l.type == 2) {
                float deg = std::clamp(l.cone * 0.5f, 1.0f, 90.0f);
                cutoff = std::cos(deg * 3.14159265f / 180.0f);
                expo = 6;
            }
            lsp[n * 4] = cutoff; lsp[n * 4 + 1] = expo; lsp[n * 4 + 2] = q;
            n++;
        }
    }
    g_vkAmb[3] = any_ambient ? 1.f : 0.f;

    ident(g_vkLightVP);
    int shadowOn = 0;
    if (g_shadows && sunSlot >= 0 && nexa_vk14::shadow_begin()) {
        int casters = 0;
        float minx = 1e9f, miny = 1e9f, minz = 1e9f, maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
        for (int s = 0; s < (int)g.entities.size(); ++s) {
            const Entity& e = g.entities[s];
            if (e.a <= 8 || !aa_kind(e) || !entity_shown(s)) continue;
            VkLocal L = vk_local(e);
            if (!L.v || L.n <= 0) continue;
            float c0 = std::sqrt(L.shape[0] * L.shape[0] + L.shape[1] * L.shape[1] + L.shape[2] * L.shape[2]);
            float c1 = std::sqrt(L.shape[4] * L.shape[4] + L.shape[5] * L.shape[5] + L.shape[6] * L.shape[6]);
            float c2 = std::sqrt(L.shape[8] * L.shape[8] + L.shape[9] * L.shape[9] + L.shape[10] * L.shape[10]);
            if (shadow_is_floor(c0, c1, c2)) continue;
            float rad = 0.5f * std::sqrt(c0 * c0 + c1 * c1 + c2 * c2);
            float x = L.shape[12], y = L.shape[13], z = L.shape[14];
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
        }
        if (casters > 0 || C.valid) {
            float half = radius * 1.1f;
            shadow_fit_box(center, half, radius);
            V3 fwd = norm(shine);
            V3 upRef = std::fabs(fwd.y) > 0.95f ? v3(0, 0, 1) : v3(0, 1, 0);
            V3 rightv = norm(cross(fwd, upRef));
            V3 up = norm(cross(rightv, fwd));
            float dist = half + 4.0f;
            V3 eye = center - fwd * dist;
            float texel = (half * 2.f) / 2048.f;
            if (texel > 1e-6f) {
                float lx = dot(eye, rightv);
                float ly = dot(eye, up);
                lx = std::floor(lx / texel) * texel;
                ly = std::floor(ly / texel) * texel;
                float lz = dot(eye, fwd);
                eye = rightv * lx + up * ly + fwd * lz;
            }
            float b2[16] = { rightv.x, up.x, -fwd.x, 0, rightv.y, up.y, -fwd.y, 0, rightv.z, up.z, -fwd.z, 0, 0, 0, 0, 1 };
            float trs[16], view[16], ortho[16];
            ident(trs);
            trs[12] = -eye.x; trs[13] = -eye.y; trs[14] = -eye.z;
            mul(view, b2, trs);
            float z0 = 0.05f, z1 = dist + half + 4.0f;
            std::memset(ortho, 0, sizeof(ortho));
            ortho[0] = 1.0f / half; ortho[5] = 1.0f / half;
            ortho[10] = -2.0f / (z1 - z0); ortho[14] = -(z1 + z0) / (z1 - z0); ortho[15] = 1;
            mul(g_vkLightVP, ortho, view);
            for (int s = 0; s < (int)g.entities.size(); ++s) {
                const Entity& e = g.entities[s];
                if (e.a <= 8 || !aa_kind(e) || !entity_shown(s)) continue;
                VkLocal L = vk_local(e);
                if (!L.v || L.n <= 0) continue;
                float mvp[16];
                mul(mvp, g_vkLightVP, L.shape);
                uint64_t off = vk_kept(L.v, L.n, 36);
                if (off != ~0ull) nexa_vk14::shadow_draw_kept(off, L.n, mvp);
                else nexa_vk14::shadow_draw(L.v, L.n, mvp);
            }
            shadowOn = 1;
        }
        nexa_vk14::shadow_end();
    }

    float ubo[268];
    std::memset(ubo, 0, sizeof(ubo));
    std::memcpy(ubo, lpos, sizeof(lpos));
    std::memcpy(ubo + 64, ldif, sizeof(ldif));
    std::memcpy(ubo + 128, lspot, sizeof(lspot));
    std::memcpy(ubo + 192, lsp, sizeof(lsp));
    ubo[256] = g_vkAmb[0]; ubo[257] = g_vkAmb[1]; ubo[258] = g_vkAmb[2]; ubo[259] = g_vkAmb[3];
    ubo[260] = g_fogR / 255.f; ubo[261] = g_fogG / 255.f; ubo[262] = g_fogB / 255.f;
    ubo[263] = (g_fog && g_fogDist > 0) ? g_fogDist * kUnit : 0.f;
    int info[4] = { n, shadowOn ? sunSlot : -1, shadowOn, 0 };
    std::memcpy(ubo + 264, info, sizeof(info));
    nexa_vk14::scene_uniforms(ubo, (int)sizeof(ubo));
    return 1;
}
