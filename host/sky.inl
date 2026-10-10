/* Scene sky. Day, sunset, and night are drawn here. Custom is six images,
   one per side: right, left, up, down, front, back. Row 0 of each image is
   the bottom, so it uploads straight to GL. */

struct SkyImg {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
};

struct SkyCpu {
    std::string stamp;
    SkyImg face[6];
};

static SkyCpu g_skyCpu;
static std::unordered_set<std::string> g_skyLogged;

static void sky_log_once(const std::string& msg) {
    if (!g_skyLogged.insert(msg).second) return;
    log_push(msg);
}

static void sky_dir(int face, float u, float v, float& x, float& y, float& z) {
    if (face == 0)      { x =  1; y =  v; z = -u; }
    else if (face == 1) { x = -1; y =  v; z =  u; }
    else if (face == 2) { x =  u; y =  1; z = -v; }
    else if (face == 3) { x =  u; y = -1; z =  v; }
    else if (face == 4) { x =  u; y =  v; z =  1; }
    else                { x = -u; y =  v; z = -1; }
}

static void sky_pick(float x, float y, float z, int& face, float& u, float& v) {
    float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    if (ax >= ay && ax >= az) {
        float m = ax > 1e-8f ? ax : 1.f;
        if (x > 0) { face = 0; u = -z / m; v =  y / m; }
        else       { face = 1; u =  z / m; v =  y / m; }
    } else if (ay >= az) {
        float m = ay > 1e-8f ? ay : 1.f;
        if (y > 0) { face = 2; u =  x / m; v = -z / m; }
        else       { face = 3; u =  x / m; v =  z / m; }
    } else {
        float m = az > 1e-8f ? az : 1.f;
        if (z > 0) { face = 4; u =  x / m; v =  y / m; }
        else       { face = 5; u = -x / m; v =  y / m; }
    }
}

static float sky_hash(int x, int y) {
    unsigned n = (unsigned)(x * 374761393 + y * 668265263);
    n = (n ^ (n >> 13)) * 1274126177u;
    return (n & 65535) / 65535.0f;
}

static float sky_noise(float x, float y) {
    int x0 = (int)std::floor(x);
    int y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    float a = sky_hash(x0, y0);
    float b = sky_hash(x0 + 1, y0);
    float c = sky_hash(x0, y0 + 1);
    float d = sky_hash(x0 + 1, y0 + 1);
    float ux = fx * fx * (3.f - 2.f * fx);
    float uy = fy * fy * (3.f - 2.f * fy);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

static int sky_chan(float c) {
    if (c < 0.f) c = 0.f;
    if (c > 255.f) c = 255.f;
    return (int)(c + 0.5f);
}

static void sky_mix(float& r, float& g, float& b, float tr, float tg, float tb, float t) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    r = r + (tr - r) * t;
    g = g + (tg - g) * t;
    b = b + (tb - b) * t;
}

static void sky_paint(int mode, float x, float y, float z, int& or_, int& og, int& ob) {
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) len = 1.f;
    x /= len; y /= len; z /= len;
    float up = y;
    if (up < 0.f) up = 0.f;
    if (up > 1.f) up = 1.f;
    float down = -y;
    if (down < 0.f) down = 0.f;
    if (down > 1.f) down = 1.f;
    up = up * up * (3.f - 2.f * up);
    down = down * down * (3.f - 2.f * down);

    float r, g, b;
    float sunx, suny, sunz, sunR, sunG, sunB;
    if (mode == 1) {
        r = 255; g = 126; b = 52;
        sky_mix(r, g, b, 52, 40, 96, up);
        sky_mix(r, g, b, 46, 28, 38, down);
        sunx = 0.78f; suny = 0.08f; sunz = 0.22f;
        sunR = 255; sunG = 214; sunB = 150;
    } else if (mode == 2) {
        r = 24; g = 28; b = 52;
        sky_mix(r, g, b, 8, 10, 24, up);
        sky_mix(r, g, b, 10, 12, 16, down);
        sunx = -0.42f; suny = 0.48f; sunz = 0.36f;
        sunR = 232; sunG = 236; sunB = 245;
    } else {
        r = 198; g = 220; b = 236;
        sky_mix(r, g, b, 70, 136, 214, up);
        sky_mix(r, g, b, 86, 114, 72, down);
        sunx = 0.42f; suny = 0.64f; sunz = 0.28f;
        sunR = 255; sunG = 248; sunB = 230;
    }
    float sl = std::sqrt(sunx * sunx + suny * suny + sunz * sunz);
    sunx /= sl; suny /= sl; sunz /= sl;
    float sd = x * sunx + y * suny + z * sunz;

    if (mode == 2) {
        int sx = (int)std::floor(x * 220.f);
        int sy = (int)std::floor(y * 220.f);
        int sz = (int)std::floor(z * 220.f);
        float star = sky_hash(sx, sy * 19 + sz);
        if (star > 0.985f && y > -0.02f) {
            float k = (star - 0.985f) / 0.015f;
            sky_mix(r, g, b, 255, 255, 255, k);
        }
        if (sd > 0.9975f) sky_mix(r, g, b, sunR, sunG, sunB, 1);
        else if (sd > 0.985f) sky_mix(r, g, b, sunR, sunG, sunB, (sd - 0.985f) / 0.0125f);
    } else {
        if (y > 0.04f) {
            float n1 = sky_noise(x * 5.5f + 2.f, z * 5.5f);
            float n2 = sky_noise(x * 13.f + 9.f, z * 13.f + 3.f);
            float cloud = n1 * 0.7f + n2 * 0.3f;
            cloud = (cloud - 0.48f) / 0.22f;
            if (cloud < 0.f) cloud = 0.f;
            if (cloud > 1.f) cloud = 1.f;
            float fade = (y - 0.04f) / 0.22f;
            if (fade > 1.f) fade = 1.f;
            cloud *= fade;
            if (mode == 1) sky_mix(r, g, b, 255, 186, 140, cloud * 0.85f);
            else sky_mix(r, g, b, 250, 252, 255, cloud);
        }
        if (sd > 0.9982f) sky_mix(r, g, b, 255, 255, 250, 1);
        else if (sd > 0.97f) sky_mix(r, g, b, sunR, sunG, sunB, (sd - 0.97f) / 0.0282f);
    }
    or_ = sky_chan(r); og = sky_chan(g); ob = sky_chan(b);
}

static void sky_bake(SkyImg& im, int mode, int face, int n) {
    im.w = n;
    im.h = n;
    im.rgb.assign((size_t)n * n * 3, 0);
    for (int j = 0; j < n; ++j) {
        float v = ((j + 0.5f) / n) * 2.f - 1.f;
        for (int i = 0; i < n; ++i) {
            float u = ((i + 0.5f) / n) * 2.f - 1.f;
            float x, y, z;
            sky_dir(face, u, v, x, y, z);
            int r, g, b;
            sky_paint(mode, x, y, z, r, g, b);
            size_t p = ((size_t)j * n + i) * 3;
            im.rgb[p] = (unsigned char)r;
            im.rgb[p + 1] = (unsigned char)g;
            im.rgb[p + 2] = (unsigned char)b;
        }
    }
}

static std::wstring sky_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

static bool sky_load_file(const std::string& path, SkyImg& im) {
    std::wstring w = sky_wide(path);
    if (w.empty()) return false;
    Gdiplus::Bitmap src(w.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok || src.GetWidth() < 1 || src.GetHeight() < 1) return false;
    int sw = (int)src.GetWidth(), sh = (int)src.GetHeight();
    int side = sw > sh ? sw : sh;
    int tw = sw, th = sh;
    if (side > 512) {
        float s = 512.f / (float)side;
        tw = (int)(sw * s);
        th = (int)(sh * s);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    Gdiplus::Bitmap bmp(tw, th, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    Gdiplus::Graphics gfx(&bmp);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
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
    return true;
}

static void sky_fill(SkyImg& im, int r, int g, int b) {
    im.w = 4;
    im.h = 4;
    im.rgb.assign(4 * 4 * 3, 0);
    for (int i = 0; i < 4 * 4; ++i) {
        im.rgb[(size_t)i * 3] = (unsigned char)r;
        im.rgb[(size_t)i * 3 + 1] = (unsigned char)g;
        im.rgb[(size_t)i * 3 + 2] = (unsigned char)b;
    }
}

static std::string sky_stamp() {
    if (g.skyMode == 1) return "sunset";
    if (g.skyMode == 2) return "night";
    if (g.skyMode == 4)
        return "solid " + std::to_string(g.clearR) + " " + std::to_string(g.clearG) + " " + std::to_string(g.clearB);
    if (g.skyMode == 3) {
        std::string s = "custom";
        for (int i = 0; i < 6; ++i) s += "\n" + g.skyFace[i];
        return s;
    }
    return "day";
}

static void sky_rebuild(const std::string& stamp) {
    g_skyCpu.stamp = stamp;
    int mode = g.skyMode;
    if (mode < 0 || mode > 4) mode = 0;
    if (mode == 4) {
        int r = g.clearR, gc = g.clearG, b = g.clearB;
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (gc < 0) gc = 0; if (gc > 255) gc = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        for (int f = 0; f < 6; ++f) sky_fill(g_skyCpu.face[f], r, gc, b);
        return;
    }
    if (mode != 3) {
        for (int f = 0; f < 6; ++f) sky_bake(g_skyCpu.face[f], mode, f, 128);
        return;
    }
    for (int f = 0; f < 6; ++f) {
        if (g.skyFace[f].empty()) {
            sky_bake(g_skyCpu.face[f], 0, f, 64);
            continue;
        }
        std::string resolved = asset_resolve(g.skyFace[f]);
        if (!sky_load_file(resolved, g_skyCpu.face[f])) {
            sky_log_once(std::string("[sky] missing: ") + g.skyFace[f]);
            sky_bake(g_skyCpu.face[f], 0, f, 64);
        }
    }
}

static const SkyCpu* sky_cpu() {
    std::string stamp = sky_stamp();
    if (g_skyCpu.stamp != stamp) sky_rebuild(stamp);
    return &g_skyCpu;
}

static void sky_sample(float x, float y, float z, int& r, int& g, int& b) {
    const SkyCpu* cpu = sky_cpu();
    int face = 0;
    float u = 0, v = 0;
    sky_pick(x, y, z, face, u, v);
    const SkyImg& im = cpu->face[face];
    if (im.w < 1 || im.h < 1 || im.rgb.empty()) { r = 28; g = 32; b = 40; return; }
    float su = (u + 1.f) * 0.5f * (im.w - 1);
    float sv = (v + 1.f) * 0.5f * (im.h - 1);
    if (su < 0) su = 0; if (su > im.w - 1) su = (float)(im.w - 1);
    if (sv < 0) sv = 0; if (sv > im.h - 1) sv = (float)(im.h - 1);
    int x0 = (int)su, y0 = (int)sv;
    int x1 = x0 + 1; if (x1 >= im.w) x1 = im.w - 1;
    int y1 = y0 + 1; if (y1 >= im.h) y1 = im.h - 1;
    float tx = su - x0, ty = sv - y0;
    r = g = b = 0;
    int out[3];
    for (int c = 0; c < 3; ++c) {
        float a = im.rgb[((size_t)y0 * im.w + x0) * 3 + c];
        float br = im.rgb[((size_t)y0 * im.w + x1) * 3 + c];
        float d = im.rgb[((size_t)y1 * im.w + x0) * 3 + c];
        float e = im.rgb[((size_t)y1 * im.w + x1) * 3 + c];
        float v0 = a + (br - a) * tx;
        float v1 = d + (e - d) * tx;
        out[c] = sky_chan(v0 + (v1 - v0) * ty);
    }
    r = out[0]; g = out[1]; b = out[2];
}
