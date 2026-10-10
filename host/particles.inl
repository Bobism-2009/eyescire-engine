/* Picture particles. An emitter object sprays its sprite, tinted by its color.
   emit_burst starts a puff with no object. High definition is the 3D cloud:
   several times as many particles, each drawn larger and more transparent. */

static const int kPartMax = 640;

struct Part {
    int alive = 0;
    int image = 0;
    int tag = -1;
    int soft = 0;
    int dim = 0; /* 1 = 3D billboard, 0 = flat view */
    int r = 255, g = 255, b = 255, a = 255;
    float x = 0, y = 0, z = 0;
    float vx = 0, vy = 0, vz = 0;
    float life = 0, maxLife = 1;
    float size = 28;
};

struct PartDraw {
    float x, y, z, size;
    int image;
    int r, g, b, a;
    int soft;
};

struct PartPix {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};

static Part g_part[kPartMax];
static unsigned g_partRng = 1;
static std::unordered_map<int, PartPix> g_partPix;

static float part_rnd() {
    g_partRng = g_partRng * 1664525u + 1013904223u;
    return (g_partRng >> 8) * (1.0f / 16777216.0f);
}

static void part_reset() {
    for (int i = 0; i < kPartMax; ++i) g_part[i].alive = 0;
    g_partPix.clear();
    for (Entity& e : g.entities) e.emitAcc = 0;
}

static int part_slot() {
    for (int i = 0; i < kPartMax; ++i) if (!g_part[i].alive) return i;
    return -1;
}

static void part_spawn(int image, int tag, float x, float y, float z, float size, float speed,
                       float spreadDeg, float life, int r, int g, int b, int a, int soft, int dim) {
    int i = part_slot();
    if (i < 0 || image == 0) return;
    float spread = spreadDeg * 0.0174532925f;
    if (spread < 0) spread = 0;
    float el = part_rnd() * spread;
    float az = part_rnd() * 6.2831853f;
    Part& p = g_part[i];
    p.alive = 1;
    p.image = image;
    p.tag = tag;
    p.soft = soft ? 1 : 0;
    p.dim = dim ? 1 : 0;
    p.r = r; p.g = g; p.b = b; p.a = a;
    p.x = x; p.y = y; p.z = z;
    p.vx = std::sin(el) * std::cos(az) * speed;
    p.vy = -std::cos(el) * speed;
    p.vz = dim ? std::sin(el) * std::sin(az) * speed : 0;
    p.life = life;
    p.maxLife = life > 0.05f ? life : 0.05f;
    p.size = size;
}

static const PartPix* part_pixels(int imageId) {
    auto found = g_partPix.find(imageId);
    if (found != g_partPix.end()) return &found->second;
    auto im = g.images.find(imageId);
    if (im == g.images.end() || !im->second.bmp) return nullptr;
    Gdiplus::Bitmap* bmp = im->second.bmp;
    int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
    if (w < 1 || h < 1) return nullptr;
    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd{};
    if (bmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return nullptr;
    PartPix pix;
    pix.w = w;
    pix.h = h;
    pix.rgba.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = (const unsigned char*)bd.Scan0 + y * bd.Stride;
        for (int x = 0; x < w; ++x) {
            size_t p = ((size_t)y * w + x) * 4;
            pix.rgba[p] = row[x * 4 + 2];
            pix.rgba[p + 1] = row[x * 4 + 1];
            pix.rgba[p + 2] = row[x * 4];
            pix.rgba[p + 3] = row[x * 4 + 3];
        }
    }
    bmp->UnlockBits(&bd);
    auto ins = g_partPix.emplace(imageId, std::move(pix));
    return &ins.first->second;
}

static int part_collect_3d(PartDraw* out, int cap) {
    int n = 0;
    for (int i = 0; i < kPartMax && n < cap; ++i) {
        const Part& p = g_part[i];
        if (!p.alive || !p.dim) continue;
        float fade = p.life / p.maxLife;
        if (fade < 0) fade = 0;
        if (fade > 1) fade = 1;
        int a = (int)(p.a * fade);
        if (p.soft) a = a * 2 / 5;
        if (a < 1) continue;
        PartDraw& d = out[n++];
        d.x = p.x; d.y = p.y; d.z = p.z;
        d.size = p.soft ? p.size * 1.35f : p.size;
        d.image = p.image;
        d.r = p.r; d.g = p.g; d.b = p.b; d.a = a;
        d.soft = p.soft;
    }
    return n;
}

static void part_tick() {
    float dt = (float)g.dtSeconds;
    if (dt < 0.001f) dt = 0.016f;
    if (dt > 0.05f) dt = 0.05f;
    int dim = g.render3d ? 1 : 0;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        Entity& e = g.entities[s];
        if (!e.active || e.kind != "emitter" || !entity_shown(s)) continue;
        if (e.sprite.empty()) continue;
        if (!e.spriteId) {
            std::string path = asset_resolve(e.sprite);
            e.spriteId = engine_image_load(path.c_str());
        }
        if (!e.spriteId) continue;
        int soft = (e.emitHD && dim) ? 1 : 0;
        float mult = soft ? 4.f : 1.f;
        int cap = soft ? 160 : 40;
        e.emitAcc += dt * (float)e.emitRate * mult;
        int guard = soft ? 24 : 8;
        while (e.emitAcc >= 1.f && guard-- > 0) {
            int have = 0;
            for (int i = 0; i < kPartMax; ++i)
                if (g_part[i].alive && g_part[i].tag == s) ++have;
            if (have >= cap) { e.emitAcc = 0; break; }
            e.emitAcc -= 1.f;
            part_spawn(e.spriteId, s,
                       e.x + e.w * 0.5f, e.y + e.h * 0.5f, (float)e.z,
                       (float)e.emitSize, (float)e.emitSpeed, (float)e.emitSpread,
                       e.emitLife / 1000.f, e.r, e.g, e.b, e.a, soft, dim);
        }
    }
    for (int i = 0; i < kPartMax; ++i) {
        Part& p = g_part[i];
        if (!p.alive) continue;
        p.life -= dt;
        if (p.life <= 0) { p.alive = 0; continue; }
        p.vy += 420.f * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;
    }
}

extern "C" int engine_emit_burst(const char* path, float x, float y, float z, int cr, int cg, int cb, int hd) {
    if (!path || !*path) return 0;
    if (cr < 0) cr = 0; if (cr > 255) cr = 255;
    if (cg < 0) cg = 0; if (cg > 255) cg = 255;
    if (cb < 0) cb = 0; if (cb > 255) cb = 255;
    std::string resolved = asset_resolve(path);
    int image = engine_image_load(resolved.c_str());
    if (!image) return 0;
    int dim = g.render3d ? 1 : 0;
    int soft = (hd && dim) ? 1 : 0;
    int n = soft ? 72 : 18;
    int made = 0;
    for (int i = 0; i < n; ++i) {
        if (part_slot() < 0) break;
        part_spawn(image, -1, x, y, z, 28, 120, 50, 0.7f, cr, cg, cb, 255, soft, dim);
        ++made;
    }
    return made;
}

extern "C" void engine_particles_draw_2d() {
    if (!g.backDC) return;
    for (int i = 0; i < kPartMax; ++i) {
        const Part& p = g_part[i];
        if (!p.alive || p.dim) continue;
        auto it = g.images.find(p.image);
        if (it == g.images.end() || !it->second.bmp) continue;
        float fade = p.life / p.maxLife;
        if (fade < 0) fade = 0;
        int a = (int)(p.a * fade);
        if (a < 1) continue;
        float zoom = g.viewZoom > 0 ? g.viewZoom : 1.f;
        int sw = (int)std::floor(p.size * zoom);
        int sh = sw;
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        int sx = g.viewX + (int)std::floor((p.x - g.viewCamX) * zoom) - sw / 2;
        int sy = g.viewY + (int)std::floor((p.y - g.viewCamY) * zoom) - sh / 2;
        Gdiplus::ColorMatrix cm{};
        cm.m[0][0] = (Gdiplus::REAL)p.r / 255.0f;
        cm.m[1][1] = (Gdiplus::REAL)p.g / 255.0f;
        cm.m[2][2] = (Gdiplus::REAL)p.b / 255.0f;
        cm.m[3][3] = (Gdiplus::REAL)a / 255.0f;
        cm.m[4][4] = 1.0f;
        Gdiplus::ImageAttributes attr;
        attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
        Gdiplus::Graphics gfx(g.backDC);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        gfx.DrawImage(it->second.bmp, Gdiplus::Rect(sx, sy, sw, sh),
                      0, 0, it->second.w, it->second.h, Gdiplus::UnitPixel, &attr, nullptr, nullptr);
    }
}
