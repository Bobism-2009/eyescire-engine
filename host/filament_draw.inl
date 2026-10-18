/* Submit one frame of the 3D scene to Filament. Shapes match the OpenGL path:
   unit box, sphere, and cylinder, plus capsules and .obj models. */

static std::vector<int> g_filSel;

static void fil_ident(float* m) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void fil_mul(float* o, const float* a, const float* b) {
    float r[16];
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1]
                + a[8 + row] * b[c * 4 + 2] + a[12 + row] * b[c * 4 + 3];
        }
    }
    std::memcpy(o, r, sizeof(r));
}

static bool fil_is_floor(float c0, float c1, float c2) {
    float a = c0, b = c1, c = c2;
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return c > 18.f && a < c * 0.08f;
}

struct FilModelCpu {
    std::vector<float> pos, nrm, uv;
};

static std::unordered_map<const Model*, FilModelCpu> g_filModels;

static const FilModelCpu& fil_model_cpu(const Model& m) {
    auto it = g_filModels.find(&m);
    if (it != g_filModels.end()) return it->second;
    FilModelCpu cpu;
    cpu.pos.reserve(m.tri.size());
    cpu.nrm.reserve(m.tri.size());
    cpu.uv.reserve(m.tri.size() / 3 * 2);
    for (size_t k = 0; k + 2 < m.tri.size(); k += 3) {
        V3 p[3], nm[3];
        for (int c = 0; c < 3; ++c) {
            p[c] = m.pos[m.tri[k + c]];
            nm[c] = (k + c < m.cn.size() && dot(m.cn[k + c], m.cn[k + c]) > 1e-8f)
                ? norm(m.cn[k + c]) : v3(0, 0, 0);
        }
        V3 face = norm(cross(p[1] - p[0], p[2] - p[0]));
        for (int c = 0; c < 3; ++c) {
            if (dot(nm[c], nm[c]) < 1e-8f) nm[c] = face;
            cpu.pos.push_back(p[c].x); cpu.pos.push_back(p[c].y); cpu.pos.push_back(p[c].z);
            cpu.nrm.push_back(nm[c].x); cpu.nrm.push_back(nm[c].y); cpu.nrm.push_back(nm[c].z);
            float u = (k + c) * 2 + 1 < m.cuv.size() ? m.cuv[(k + c) * 2] : 0;
            float v = (k + c) * 2 + 1 < m.cuv.size() ? m.cuv[(k + c) * 2 + 1] : 0;
            cpu.uv.push_back(u); cpu.uv.push_back(v);
        }
    }
    auto ins = g_filModels.emplace(&m, std::move(cpu));
    return ins.first->second;
}

/* Height maps on primitives displace a dense mesh along the surface normal,
   so the silhouette is bumpy too. Normal maps alone only change shading. */
struct FilHeightGrid {
    int w = 0, h = 0;
    std::vector<float> v;
};

static std::unordered_map<const void*, FilHeightGrid> g_filHeights;

/* Box-filtered down to at most 512 so a vertex samples an area, not one texel. */
static const FilHeightGrid& fil_height_grid(const MapImg& im) {
    FilHeightGrid& g = g_filHeights[im.rgb.data()];
    if (!g.v.empty()) return g;
    int f = 1;
    while (im.w / f > 512 || im.h / f > 512) f *= 2;
    g.w = std::max(1, im.w / f);
    g.h = std::max(1, im.h / f);
    g.v.assign((size_t)g.w * g.h, 0.f);
    for (int y = 0; y < g.h; ++y) {
        for (int x = 0; x < g.w; ++x) {
            float s = 0;
            int n = 0;
            for (int yy = y * f; yy < std::min(im.h, (y + 1) * f); ++yy)
                for (int xx = x * f; xx < std::min(im.w, (x + 1) * f); ++xx, ++n)
                    s += im.rgb[((size_t)yy * im.w + xx) * 3];
            g.v[(size_t)y * g.w + x] = n ? s / (255.f * n) : 0.f;
        }
    }
    return g;
}

/* Same texel the GPU reads at (u, v). The bridge uploads rows flipped and its
   materials flip V back, so v indexes MapImg rows directly (row 0 = v 0). */
static float fil_height_at(const FilHeightGrid& g, float u, float v) {
    u -= std::floor(u);
    v -= std::floor(v);
    float x = u * g.w - 0.5f, y = v * g.h - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy) {
        xx %= g.w; if (xx < 0) xx += g.w;
        yy %= g.h; if (yy < 0) yy += g.h;
        return g.v[(size_t)yy * g.w + xx];
    };
    float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
    float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
    return a + (b - a) * fy;
}

struct FilDisp {
    int kind = 0;
    const void* height = nullptr;
    int hw = 0, hh = 0;
    float tile = 0, amp = 0;
    float rise = 0; /* furthest any vertex moved outward, unit space */
    FilModelCpu cpu;
};

static std::unordered_map<int, std::unique_ptr<FilDisp>> g_filDisp;

/* Unit-space surface point for patch (face) at grid coords s, t in [0, 1].
   Positions, normals and UVs match the bridge's unit meshes. */
static void fil_disp_eval(int kind, int face, float s, float t, V3& p, V3& n, float& u, float& v) {
    if (kind == 2) {
        static const float B[6][9] = {
            { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { -1, 0, 0, 0, 1, 0, 0, 0, 1 },
            { 0, 1, 0, 1, 0, 0, 0, 0, 1 }, { 0, -1, 0, 1, 0, 0, 0, 0, 1 },
            { 0, 0, 1, 1, 0, 0, 0, 1, 0 }, { 0, 0, -1, 1, 0, 0, 0, 1, 0 },
        };
        const float* b = B[face];
        float ss = std::tan((s * 2.f - 1.f) * 0.78539816f);
        float tt = std::tan((t * 2.f - 1.f) * 0.78539816f);
        n = norm(v3(b[0] + b[3] * ss + b[6] * tt, b[1] + b[4] * ss + b[7] * tt, b[2] + b[5] * ss + b[8] * tt));
        p = n * 0.5f;
        float sg = (face & 1) ? -1.f : 1.f;
        if (face < 2) { u = sg * p.z + 0.5f; v = p.y + 0.5f; }
        else if (face < 4) { u = p.x + 0.5f; v = sg * p.z + 0.5f; }
        else { u = sg * p.x + 0.5f; v = p.y + 0.5f; }
    } else if (kind == 1) {
        static const float F[6][4][3] = {
            { { .5f,-.5f,-.5f},{ .5f, .5f,-.5f},{ .5f, .5f, .5f},{ .5f,-.5f, .5f} },
            { {-.5f,-.5f, .5f},{-.5f, .5f, .5f},{-.5f, .5f,-.5f},{-.5f,-.5f,-.5f} },
            { {-.5f, .5f,-.5f},{-.5f, .5f, .5f},{ .5f, .5f, .5f},{ .5f, .5f,-.5f} },
            { {-.5f,-.5f, .5f},{-.5f,-.5f,-.5f},{ .5f,-.5f,-.5f},{ .5f,-.5f, .5f} },
            { {-.5f,-.5f, .5f},{ .5f,-.5f, .5f},{ .5f, .5f, .5f},{-.5f, .5f, .5f} },
            { { .5f,-.5f,-.5f},{-.5f,-.5f,-.5f},{-.5f, .5f,-.5f},{ .5f, .5f,-.5f} },
        };
        static const float N[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
        const float (*c)[3] = F[face];
        V3 c0 = v3(c[0][0], c[0][1], c[0][2]);
        V3 c1 = v3(c[1][0], c[1][1], c[1][2]);
        V3 c3 = v3(c[3][0], c[3][1], c[3][2]);
        p = c0 + (c3 - c0) * s + (c1 - c0) * t;
        n = v3(N[face][0], N[face][1], N[face][2]);
        u = s; v = t;
    } else {
        float a = s * 6.2831853f, ca = std::cos(a), sa = std::sin(a);
        if (face == 0) {
            p = v3(ca * .5f, t - .5f, sa * .5f);
            n = v3(ca, 0, sa);
            u = s; v = t;
        } else {
            float y = face == 1 ? .5f : -.5f;
            p = v3(ca * .5f * t, y, sa * .5f * t);
            n = v3(0, y * 2.f, 0);
            u = ca * .5f * t + .5f; v = sa * .5f * t + .5f;
        }
    }
}

static void fil_disp_build(FilDisp& d, const FilHeightGrid& hg) {
    struct Patch { int face, nu, nv, base; };
    std::vector<Patch> patches;
    if (d.kind == 2) for (int f = 0; f < 6; ++f) patches.push_back({ f, 112, 112, 0 });
    else if (d.kind == 1) for (int f = 0; f < 6; ++f) patches.push_back({ f, 96, 96, 0 });
    else { patches.push_back({ 0, 192, 96, 0 }); patches.push_back({ 1, 192, 24, 0 }); patches.push_back({ 2, 192, 24, 0 }); }
    std::vector<V3> P, N0, D;
    std::vector<float> UV;
    for (Patch& pt : patches) {
        pt.base = (int)P.size();
        for (int j = 0; j <= pt.nv; ++j) {
            for (int i = 0; i <= pt.nu; ++i) {
                V3 p, n;
                float u, v;
                fil_disp_eval(d.kind, pt.face, (float)i / pt.nu, (float)j / pt.nv, p, n, u, v);
                float h = fil_height_at(hg, u * d.tile, v * d.tile);
                d.rise = std::max(d.rise, (h - 0.5f) * d.amp);
                P.push_back(p);
                N0.push_back(n);
                D.push_back(p + n * ((h - 0.5f) * d.amp));
                UV.push_back(u);
                UV.push_back(v);
            }
        }
    }
    /* Patch edges meet at the same point with different UVs. Averaging the
       displaced copies of a point keeps those seams closed. */
    std::unordered_map<uint64_t, std::vector<int>> groups;
    auto key = [](V3 p) {
        auto q = [](float f) { return (uint64_t)((int64_t)std::lround(f * 65536.f) + (1 << 20)) & 0x1FFFFF; };
        return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
    };
    for (int i = 0; i < (int)P.size(); ++i) groups[key(P[i])].push_back(i);
    for (auto& kv : groups) {
        if (kv.second.size() < 2) continue;
        V3 s = v3(0, 0, 0);
        for (int i : kv.second) s = s + D[i];
        s = s * (1.f / kv.second.size());
        for (int i : kv.second) D[i] = s;
    }
    std::vector<V3> NN(P.size());
    for (const Patch& pt : patches) {
        int row = pt.nu + 1;
        for (int j = 0; j <= pt.nv; ++j) {
            for (int i = 0; i <= pt.nu; ++i) {
                int k = pt.base + j * row + i;
                V3 du = D[pt.base + j * row + std::min(i + 1, pt.nu)] - D[pt.base + j * row + std::max(i - 1, 0)];
                V3 dv = D[pt.base + std::min(j + 1, pt.nv) * row + i] - D[pt.base + std::max(j - 1, 0) * row + i];
                V3 c = cross(du, dv);
                if (dot(c, c) < 1e-14f) c = N0[k];
                else if (dot(c, N0[k]) < 0) c = c * -1.f;
                NN[k] = norm(c);
            }
        }
    }
    /* Smooth across seams, but keep a box's edges hard. */
    std::vector<V3> NS = NN;
    for (auto& kv : groups) {
        if (kv.second.size() < 2) continue;
        for (int i : kv.second) {
            V3 s = v3(0, 0, 0);
            for (int j : kv.second) if (dot(N0[i], N0[j]) > 0.7f) s = s + NN[j];
            NS[i] = norm(s);
        }
    }
    FilModelCpu& cpu = d.cpu;
    cpu = {};
    auto emit = [&](int k) {
        cpu.pos.push_back(D[k].x); cpu.pos.push_back(D[k].y); cpu.pos.push_back(D[k].z);
        cpu.nrm.push_back(NS[k].x); cpu.nrm.push_back(NS[k].y); cpu.nrm.push_back(NS[k].z);
        cpu.uv.push_back(UV[k * 2]); cpu.uv.push_back(UV[k * 2 + 1]);
    };
    auto tri = [&](int a, int b, int c) {
        V3 f = cross(P[b] - P[a], P[c] - P[a]);
        if (dot(f, f) < 1e-16f) return;
        if (dot(f, N0[a] + N0[b] + N0[c]) < 0) std::swap(b, c);
        emit(a); emit(b); emit(c);
    };
    for (const Patch& pt : patches) {
        int row = pt.nu + 1;
        for (int j = 0; j < pt.nv; ++j) {
            for (int i = 0; i < pt.nu; ++i) {
                int a = pt.base + j * row + i, b = a + 1, c = a + row + 1, e = a + row;
                tri(a, b, c);
                tri(a, c, e);
            }
        }
    }
}

static const FilDisp* fil_disp_for(int slot, int kind, const MapImg& height, float tile, float amp) {
    std::unique_ptr<FilDisp>& slotDisp = g_filDisp[slot];
    if (!slotDisp) slotDisp = std::make_unique<FilDisp>();
    FilDisp& d = *slotDisp;
    if (d.kind == kind && d.height == height.rgb.data() && d.hw == height.w && d.hh == height.h
        && d.tile == tile && d.amp == amp && !d.cpu.pos.empty())
        return &d;
    if (!d.cpu.pos.empty() && p_fb_model_forget) p_fb_model_forget(&d);
    d.kind = kind;
    d.height = height.rgb.data();
    d.hw = height.w;
    d.hh = height.h;
    d.tile = tile;
    d.amp = amp;
    d.rise = 0;
    fil_disp_build(d, fil_height_grid(height));
    return &d;
}

static bool fil_shape(const Entity& e, float* shape, int* kind, const Model** model) {
    *model = nullptr;
    *kind = 1;
    float w = (float)e.w, h = (float)e.h, d = (float)depth_of(e);
    if (w <= 0 || h <= 0) return false;
    float sx, sy, sz;
    if (e.kind == "sphere") {
        float dm = std::min(w, h) * kUnit;
        sx = sy = sz = dm;
        *kind = 2;
    } else if (e.kind == "cylinder") {
        sx = w * kUnit; sy = h * kUnit; sz = w * kUnit;
        *kind = 3;
    } else if (e.kind == "capsule") {
        sx = w * kUnit; sy = h * kUnit; sz = w * kUnit;
        *kind = 4;
    } else {
        const Model* m = nullptr;
        if (e.kind == "model" && !e.model.empty()) m = &model_load(model_path(e.model));
        if (m && m->ok) {
            float s = std::max(w, h) * kUnit;
            sx = sy = sz = s;
            *kind = 5;
            *model = m;
        } else {
            sx = w * kUnit; sy = h * kUnit; sz = d * kUnit;
        }
    }
    float t[16], r[16], sc[16], tr[16];
    fil_ident(t); fil_ident(sc);
    t[12] = (e.x + w * 0.5f) * kUnit;
    t[13] = -(e.y + h * 0.5f) * kUnit;
    t[14] = -(e.z + d * 0.5f) * kUnit;
    ::orient_matrix(r, e.rotX, e.rotY, e.rotDeg);
    sc[0] = sx; sc[5] = sy; sc[10] = sz;
    fil_mul(tr, t, r);
    fil_mul(shape, tr, sc);
    return true;
}

static void fil_entity(const Entity& e, int slot, int outline, int or_, int og, int ob) {
    if (!p_fb_prim || !p_fb_model || !p_fb_tris) return;
    float shape[16];
    int kind = 1;
    const Model* model = nullptr;
    if (!fil_shape(e, shape, &kind, &model)) return;
    int r = outline ? or_ : e.r;
    int g = outline ? og : e.g;
    int b = outline ? ob : e.b;
    int a = outline ? 255 : std::clamp(e.a, 0, 255);
    if (outline) {
        /* A fixed scale (the old 3%) is a hairline on a small prop and a wide
           band on a large box. Push each axis out by ~1.5 pixels instead. */
        V3 center = v3(shape[12], shape[13], shape[14]);
        V3 gap = center - C.eye;
        float dist = std::sqrt(dot(gap, gap));
        if (dist < 0.05f) dist = 0.05f;
        float pad = 1.5f * dist / (C.f > 1.f ? C.f : 1.f);
        auto dit = g_filDisp.find(slot);
        if (kind >= 1 && kind <= 3 && !e.texHeight.empty() && e.matBump > 0
            && dit != g_filDisp.end() && dit->second && dit->second->kind == kind) {
            float ax = std::sqrt(shape[0] * shape[0] + shape[1] * shape[1] + shape[2] * shape[2]);
            pad += dit->second->rise * ax;
        }
        auto inflate = [&](int i) {
            float len = std::sqrt(shape[i] * shape[i] + shape[i + 1] * shape[i + 1] + shape[i + 2] * shape[i + 2]);
            if (len < 1e-5f) return;
            float s = (len + pad) / len;
            shape[i] *= s; shape[i + 1] *= s; shape[i + 2] *= s;
        };
        inflate(0); inflate(4); inflate(8);
    } else if (e.selected) {
        g_filSel.push_back(slot);
    }
    float c0 = std::sqrt(shape[0] * shape[0] + shape[1] * shape[1] + shape[2] * shape[2]);
    float c1 = std::sqrt(shape[4] * shape[4] + shape[5] * shape[5] + shape[6] * shape[6]);
    float c2 = std::sqrt(shape[8] * shape[8] + shape[9] * shape[9] + shape[10] * shape[10]);
    int cast = (!outline && a >= 250 && g_shadows && !fil_is_floor(c0, c1, c2)) ? 1 : 0;
    int mode = outline ? 2 : 0;
    const MapImg* baseM = nullptr;
    const MapImg* nrmM = nullptr;
    const MapImg* metalM = nullptr;
    const MapImg* roughM = nullptr;
    const MapImg* heightM = nullptr;
    bool haveMaps = false;
    if (!outline) {
        if (kind == 5 && model) {
            if (ent_has_tex(e)) {
                const EntMapSet* maps = ent_maps_for_draw(slot, e, model);
                if (maps) {
                    baseM = &maps->base; nrmM = &maps->nrm; metalM = &maps->metal;
                    roughM = &maps->rough; heightM = &maps->height;
                    haveMaps = ent_maps_any(*maps);
                }
            } else {
                baseM = &model->base; nrmM = &model->nrm; metalM = &model->metal;
                roughM = &model->rough; heightM = &model->height;
                haveMaps = model->base.w > 0 || model->nrm.w > 0 || model->metal.w > 0
                        || model->rough.w > 0 || model->height.w > 0;
            }
        } else if (ent_has_tex(e)) {
            const EntMapSet* maps = ent_maps_for_draw(slot, e, nullptr);
            if (maps) {
                baseM = &maps->base; nrmM = &maps->nrm; metalM = &maps->metal;
                roughM = &maps->rough; heightM = &maps->height;
                haveMaps = ent_maps_any(*maps);
            }
        }
    }
    auto map_ptr = [](const MapImg* im) -> const unsigned char* {
        return im && im->w > 0 ? im->rgb.data() : nullptr;
    };
    auto map_w = [](const MapImg* im) { return im ? im->w : 0; };
    auto map_h = [](const MapImg* im) { return im ? im->h : 0; };
    /* Albedo map already has color; multiply by entity tint would wash it to flat orange. */
    if (!outline && map_ptr(baseM)) {
        r = 255;
        g = 255;
        b = 255;
    }
    if (!outline && p_fb_mat_params) {
        float rough = (float)e.matRough / 100.f;
        float metal = (float)e.matMetal / 100.f;
        float bump = (float)e.matBump / 100.f;
        if (rough < 0.04f) rough = 0.04f;
        if (rough > 1.f) rough = 1.f;
        if (metal < 0.f) metal = 0.f;
        if (metal > 1.f) metal = 1.f;
        if (bump < 0.f) bump = 0.f;
        if (bump > 4.f) bump = 4.f;
        /* Mesh UVs, not a three-face blend. The blend smears a sphere into stripes. */
        p_fb_mat_params(rough, metal, bump, (float)e.matTile, 0.f);
    }
    if (kind == 4) {
        CapsuleLocal cap = capsule_local((float)e.w, (float)e.h);
        float sx = shape[0], sy = shape[5], sz = shape[10];
        (void)sx; (void)sy; (void)sz;
        std::vector<float> pos, nrm;
        capsule_emit(cap, 1, 1, 1, [&](float px, float py, float pz, float nx, float ny, float nz) {
            float n2 = nx * nx + ny * ny + nz * nz;
            float inv = n2 > 1e-8f ? 1.0f / std::sqrt(n2) : 0;
            pos.push_back(px); pos.push_back(py); pos.push_back(pz);
            nrm.push_back(nx * inv); nrm.push_back(ny * inv); nrm.push_back(nz * inv);
        });
        if (pos.size() >= 9)
            p_fb_tris(pos.data(), nrm.data(), (int)pos.size() / 3, shape, r, g, b, a,
                      mode == 0, outline ? 2 : 1, mode == 0, cast);
        return;
    }
    if (kind == 5 && model) {
        const FilModelCpu& cpu = fil_model_cpu(*model);
        if (cpu.pos.empty()) return;
        if (outline) {
            std::vector<float> wp, wn;
            wp.reserve(cpu.pos.size());
            wn.reserve(cpu.nrm.size());
            int n = (int)cpu.pos.size() / 3;
            for (int i = 0; i < n; ++i) {
                float x = cpu.pos[i * 3], y = cpu.pos[i * 3 + 1], z = cpu.pos[i * 3 + 2];
                wp.push_back(shape[0] * x + shape[4] * y + shape[8] * z + shape[12]);
                wp.push_back(shape[1] * x + shape[5] * y + shape[9] * z + shape[13]);
                wp.push_back(shape[2] * x + shape[6] * y + shape[10] * z + shape[14]);
                float nx = cpu.nrm[i * 3], ny = cpu.nrm[i * 3 + 1], nz = cpu.nrm[i * 3 + 2];
                wn.push_back(nx); wn.push_back(ny); wn.push_back(nz);
            }
            float id[16];
            fil_ident(id);
            p_fb_tris(wp.data(), wn.data(), n, id, r, g, b, a, 0, 2, 0, 0);
            return;
        }
        p_fb_model(model, cpu.pos.data(), cpu.nrm.data(), cpu.uv.data(), (int)cpu.pos.size() / 3,
                   shape, r, g, b, a, cast,
                   map_ptr(baseM), map_w(baseM), map_h(baseM),
                   map_ptr(nrmM), map_w(nrmM), map_h(nrmM),
                   map_ptr(metalM), map_w(metalM), map_h(metalM),
                   map_ptr(roughM), map_w(roughM), map_h(roughM),
                   map_ptr(heightM), map_w(heightM), map_h(heightM));
        return;
    }
    float bumpAmt = std::clamp((float)e.matBump / 100.f, 0.f, 4.f);
    if (haveMaps && mode == 0 && kind >= 1 && kind <= 3 && map_ptr(heightM) && bumpAmt > 0.01f
        && p_fb_model && p_fb_model_forget) {
        float tile = std::clamp((float)e.matTile, 0.25f, 32.f);
        const FilDisp* disp = fil_disp_for(slot, kind, *heightM, tile, 0.05f * bumpAmt);
        const FilModelCpu& cpu = disp->cpu;
        if (!cpu.pos.empty()) {
            p_fb_model(disp, cpu.pos.data(), cpu.nrm.data(), cpu.uv.data(), (int)cpu.pos.size() / 3,
                       shape, r, g, b, a, cast,
                       map_ptr(baseM), map_w(baseM), map_h(baseM),
                       map_ptr(nrmM), map_w(nrmM), map_h(nrmM),
                       map_ptr(metalM), map_w(metalM), map_h(metalM),
                       map_ptr(roughM), map_w(roughM), map_h(roughM),
                       map_ptr(heightM), map_w(heightM), map_h(heightM));
            return;
        }
    }
    if (haveMaps && p_fb_prim_maps && mode == 0) {
        p_fb_prim_maps(kind, shape, r, g, b, a, cast, mode,
                       map_ptr(baseM), map_w(baseM), map_h(baseM),
                       map_ptr(nrmM), map_w(nrmM), map_h(nrmM),
                       map_ptr(metalM), map_w(metalM), map_h(metalM),
                       map_ptr(roughM), map_w(roughM), map_h(roughM),
                       map_ptr(heightM), map_w(heightM), map_h(heightM));
        return;
    }
    p_fb_prim(kind, shape, r, g, b, a, cast, mode);
}

static void fil_particles() {
    if (!p_fb_sprite) return;
    PartDraw buf[kPartMax];
    int n = part_collect_3d(buf, kPartMax);
    for (int i = 0; i < n; ++i) {
        const PartDraw& d = buf[i];
        const PartPix* pix = part_pixels(d.image);
        if (!pix || pix->w < 1) continue;
        float cx = d.x * kUnit, cy = -d.y * kUnit, cz = -d.z * kUnit;
        V3 c = v3(cx, cy, cz), p0, p1, p2, p3;
        if (d.streak) {
            V3 along = v3(0, -1, 0);
            V3 across = cross(C.fwd, along);
            if (dot(across, across) < 1e-8f) across = C.right;
            across = norm(across);
            float hw = d.size * kUnit * 0.06f;
            float hh = d.size * kUnit * 0.55f;
            if (hw < 0.01f) hw = 0.01f;
            p0 = c - across * hw - along * hh;
            p1 = c + across * hw - along * hh;
            p2 = c + across * hw + along * hh;
            p3 = c - across * hw + along * hh;
        } else {
            float hs = d.size * kUnit * 0.5f;
            p0 = c - C.right * hs - C.up * hs;
            p1 = c + C.right * hs - C.up * hs;
            p2 = c + C.right * hs + C.up * hs;
            p3 = c - C.right * hs + C.up * hs;
        }
        float pos[12] = { p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z, p3.x, p3.y, p3.z };
        /* Row 0 of the picture is the top. v = 0 is that row, so the top of the quad uses it. */
        float uv[8] = { 0, 1, 1, 1, 1, 0, 0, 0 };
        p_fb_sprite(pix, pix->rgba.data(), pix->w, pix->h, pos, uv, d.r, d.g, d.b, d.a);
    }
}

static void fil_outline(int or_, int og, int ob) {
    for (int s : g_filSel) {
        if (!active_slot(s)) continue;
        fil_entity(g.entities[s], s, 1, or_, og, ob);
    }
}

static float fil_peak(float r, float g, float b) {
    float m = r;
    if (g > m) m = g;
    if (b > m) m = b;
    return m < 1e-4f ? 1e-4f : m;
}

static void fil_lights() {
    if (!p_fb_sun || !p_fb_ambient) return;
    int anyAmb = 0;
    float ar = 0.42f, ag = 0.42f, ab = 0.44f;
    int sun = 0;
    for (const Light& l : g_lights) {
        if (l.type == 3) {
            if (!anyAmb) { ar = ag = ab = 0; anyAmb = 1; }
            ar += l.r; ag += l.g; ab += l.b;
        }
    }
    p_fb_ambient(ar, ag, ab);
    for (const Light& l : g_lights) {
        float m = fil_peak(l.r, l.g, l.b);
        float cr = l.r / m, cg = l.g / m, cb = l.b / m;
        if (l.type == 0) {
            p_fb_sun(l.dir.x, l.dir.y, l.dir.z, cr, cg, cb, m * 32000.0f, sun == 0 && g_shadows);
            sun = 1;
        } else if (l.type == 1 && p_fb_point) {
            float range = l.range > 0.05f ? l.range : 0.05f;
            p_fb_point(l.pos.x, l.pos.y, l.pos.z, cr, cg, cb, m * 600.0f * range * range, range);
        } else if (l.type == 2 && p_fb_spot) {
            float range = l.range > 0.05f ? l.range : 0.05f;
            float outer = std::clamp(l.cone * 0.5f, 1.0f, 89.0f) * 0.0174532925f;
            float inner = outer * 0.65f;
            p_fb_spot(l.pos.x, l.pos.y, l.pos.z, l.dir.x, l.dir.y, l.dir.z,
                      cr, cg, cb, m * 800.0f * range * range, range, inner, outer);
        }
    }
}

static void fil_sky_submit() {
    if (!p_fb_sky) return;
    const SkyCpu* sky = sky_cpu();
    if (!sky || sky->face[0].w < 1 || sky->face[0].rgb.empty()) {
        p_fb_sky(nullptr, nullptr, nullptr);
        return;
    }
    const unsigned char* faces[6];
    int wh[12];
    for (int i = 0; i < 6; ++i) {
        faces[i] = sky->face[i].rgb.empty() ? nullptr : sky->face[i].rgb.data();
        wh[i * 2] = sky->face[i].w;
        wh[i * 2 + 1] = sky->face[i].h;
    }
    p_fb_sky(faces, wh, sky->stamp.c_str());
}

static int fil_frame(int w, int h, double fov, int br, int bg, int bb) {
    if (!p_fb_begin) return 0;
    g_filSel.clear();
    float aspect = h > 0 ? (float)w / (float)h : 1.f;
    float dist = (g_fog && g_fogDist > 0) ? g_fogDist * kUnit : 0.f;
    p_fb_begin(C.eye.x, C.eye.y, C.eye.z, C.target.x, C.target.y, C.target.z,
               C.up.x, C.up.y, C.up.z, (float)fov, aspect, w, h, br, bg, bb,
               g_aa, g_shadows, g_fog, g_fogR, g_fogG, g_fogB, dist);
    fil_sky_submit();
    fil_lights();
    return 1;
}
