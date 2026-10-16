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
        for (int i = 0; i < 12; ++i) shape[i] *= 1.03f;
    } else if (e.selected) {
        g_filSel.push_back(slot);
    }
    float c0 = std::sqrt(shape[0] * shape[0] + shape[1] * shape[1] + shape[2] * shape[2]);
    float c1 = std::sqrt(shape[4] * shape[4] + shape[5] * shape[5] + shape[6] * shape[6]);
    float c2 = std::sqrt(shape[8] * shape[8] + shape[9] * shape[9] + shape[10] * shape[10]);
    int cast = (!outline && a >= 250 && g_shadows && !fil_is_floor(c0, c1, c2)) ? 1 : 0;
    int mode = outline ? 2 : 0;
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
        const unsigned char* al = model->base.w > 0 ? model->base.rgb.data() : nullptr;
        const unsigned char* nm = model->nrm.w > 0 ? model->nrm.rgb.data() : nullptr;
        const unsigned char* mt = model->metal.w > 0 ? model->metal.rgb.data() : nullptr;
        const unsigned char* rh = model->rough.w > 0 ? model->rough.rgb.data() : nullptr;
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
                   al, model->base.w, model->base.h,
                   nm, model->nrm.w, model->nrm.h,
                   mt, model->metal.w, model->metal.h,
                   rh, model->rough.w, model->rough.h);
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
