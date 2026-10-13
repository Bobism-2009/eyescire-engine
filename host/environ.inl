/* Day/night and rain/snow. Scene-level state driven from play (physics_step)
   and script APIs; the 3D view applies wetness/snow tint to meshes. */

struct EnvState {
    int dayCycle = 0;
    int dayPaused = 0;
    float dayTime = 0.35f;   /* 0 midnight .. 0.25 sunrise .. 0.5 noon .. 0.75 sunset */
    float dayLength = 120.f; /* seconds for a full loop */

    int weather = 0;           /* 0 none, 1 rain, 2 snow */
    int weatherIntensity = 50; /* 0..100 */
    float wetness = 0.f;       /* 0..1, lerps toward rain */
    float snowCover = 0.f;     /* 0..1, lerps toward snow */

    float camSx = 640.f, camSy = 360.f, camSz = 0.f; /* last 3D focus, scene px */
    int rainImg = 0;
    int snowImg = 0;
    float weatherAcc = 0.f;
};

static EnvState g_env;

static float env_clampf(float v, float a, float b) {
    if (v < a) return a;
    if (v > b) return b;
    return v;
}

static float env_lerp(float a, float b, float t) {
    return a + (b - a) * env_clampf(t, 0.f, 1.f);
}

static void env_lerp3(int& r, int& g, int& b, int r0, int g0, int b0, int r1, int g1, int b1, float t) {
    r = (int)std::lround(env_lerp((float)r0, (float)r1, t));
    g = (int)std::lround(env_lerp((float)g0, (float)g1, t));
    b = (int)std::lround(env_lerp((float)b0, (float)b1, t));
}

/* Keyframes: midnight, sunrise, noon, sunset, midnight. */
static void env_day_sample(float t, int& sunR, int& sunG, int& sunB, int& sunI,
                           int& ambR, int& ambG, int& ambB, int& ambI,
                           int& clearR, int& clearG, int& clearB,
                           float& rotDeg, float& tilt) {
    while (t < 0.f) t += 1.f;
    while (t >= 1.f) t -= 1.f;
    struct Key {
        float u;
        int sr, sg, sb, si;
        int ar, ag, ab, ai;
        int cr, cg, cb;
        float rot, tilt;
    };
    static const Key keys[] = {
        { 0.00f,  40,  50,  90,  8,   20,  28,  48,  35,  12,  16,  28,  180.f, -25.f },
        { 0.22f, 255, 160,  90, 45,   70,  55,  80,  45,  70,  55,  90,  100.f,   8.f },
        { 0.35f, 255, 230, 180, 70,   90, 100, 120,  55, 140, 176, 214,  150.f,  35.f },
        { 0.50f, 255, 244, 214, 85,  110, 120, 140,  60, 160, 190, 230,  180.f,  70.f },
        { 0.65f, 255, 210, 150, 70,   95, 100, 120,  55, 150, 160, 190,  210.f,  35.f },
        { 0.78f, 255, 120,  70, 40,   80,  50,  60,  45,  90,  45,  55,  260.f,   8.f },
        { 0.90f,  50,  55,  95, 12,   25,  30,  50,  38,  18,  20,  36,  300.f, -15.f },
        { 1.00f,  40,  50,  90,  8,   20,  28,  48,  35,  12,  16,  28,  180.f, -25.f },
    };
    const int n = (int)(sizeof(keys) / sizeof(keys[0]));
    int i = 0;
    while (i + 1 < n && t > keys[i + 1].u) ++i;
    const Key& a = keys[i];
    const Key& b = keys[i + 1 < n ? i + 1 : i];
    float span = b.u - a.u;
    float u = span > 1e-5f ? (t - a.u) / span : 0.f;
    env_lerp3(sunR, sunG, sunB, a.sr, a.sg, a.sb, b.sr, b.sg, b.sb, u);
    env_lerp3(ambR, ambG, ambB, a.ar, a.ag, a.ab, b.ar, b.ag, b.ab, u);
    env_lerp3(clearR, clearG, clearB, a.cr, a.cg, a.cb, b.cr, b.cg, b.cb, u);
    sunI = (int)std::lround(env_lerp((float)a.si, (float)b.si, u));
    ambI = (int)std::lround(env_lerp((float)a.ai, (float)b.ai, u));
    rotDeg = env_lerp(a.rot, b.rot, u);
    tilt = env_lerp(a.tilt, b.tilt, u);
}

static int env_find_kind(const char* kind) {
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (!g.entities[s].active) continue;
        if (g.entities[s].kind == kind) return s;
    }
    return -1;
}

static void env_apply_day() {
    if (!g_env.dayCycle) return;
    int sunR, sunG, sunB, sunI, ambR, ambG, ambB, ambI, cr, cg, cb;
    float rot, tilt;
    env_day_sample(g_env.dayTime, sunR, sunG, sunB, sunI, ambR, ambG, ambB, ambI, cr, cg, cb, rot, tilt);

    /* Weather dims the sun a bit. */
    float dim = 1.f - 0.35f * g_env.wetness - 0.15f * g_env.snowCover;
    sunI = (int)std::lround(sunI * dim);
    if (sunI < 0) sunI = 0;

    int sun = env_find_kind("light_directional");
    if (sun >= 0) {
        Entity& e = g.entities[sun];
        e.r = sunR; e.g = sunG; e.b = sunB;
        e.lightIntensity = sunI;
        e.rotDeg = (int)std::lround(rot) % 360;
        if (e.rotDeg < 0) e.rotDeg += 360;
        e.lightTilt = (int)std::lround(tilt);
        if (e.lightTilt < -90) e.lightTilt = -90;
        if (e.lightTilt > 90) e.lightTilt = 90;
    }
    int amb = env_find_kind("light_ambient");
    if (amb >= 0) {
        Entity& e = g.entities[amb];
        e.r = ambR; e.g = ambG; e.b = ambB;
        e.lightIntensity = ambI;
    }
    /* Continuous sky while the cycle runs. */
    g.skyMode = 4;
    g.clearR = cr; g.clearG = cg; g.clearB = cb;
    if (g_env.wetness > 0.05f) {
        float w = g_env.wetness;
        g.clearR = (int)std::lround(env_lerp((float)g.clearR, 70.f, w * 0.55f));
        g.clearG = (int)std::lround(env_lerp((float)g.clearG, 80.f, w * 0.55f));
        g.clearB = (int)std::lround(env_lerp((float)g.clearB, 95.f, w * 0.55f));
    }
    if (g_env.snowCover > 0.05f) {
        float s = g_env.snowCover;
        g.clearR = (int)std::lround(env_lerp((float)g.clearR, 200.f, s * 0.45f));
        g.clearG = (int)std::lround(env_lerp((float)g.clearG, 210.f, s * 0.45f));
        g.clearB = (int)std::lround(env_lerp((float)g.clearB, 220.f, s * 0.45f));
    }
}

static int env_make_rgba_image(int w, int h, const std::vector<unsigned char>& rgba) {
    if (w < 1 || h < 1 || (int)rgba.size() < w * h * 4) return 0;
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return 0; }
    Gdiplus::BitmapData bd{};
    Gdiplus::Rect rc(0, 0, w, h);
    if (bmp->LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) {
        delete bmp;
        return 0;
    }
    for (int y = 0; y < h; ++y) {
        unsigned char* row = (unsigned char*)bd.Scan0 + y * bd.Stride;
        for (int x = 0; x < w; ++x) {
            size_t p = ((size_t)y * w + x) * 4;
            row[x * 4 + 0] = rgba[p + 2];
            row[x * 4 + 1] = rgba[p + 1];
            row[x * 4 + 2] = rgba[p + 0];
            row[x * 4 + 3] = rgba[p + 3];
        }
    }
    bmp->UnlockBits(&bd);
    int id = g.nextImageId++;
    Image img;
    img.bmp = bmp;
    img.w = w;
    img.h = h;
    image_ensure_dib(img);
    g.images[id] = img;
    return id;
}

static int g_weatherSpriteVer = 0;

static void env_ensure_weather_sprites() {
    const int ver = 3;
    if (g_weatherSpriteVer != ver) {
        g_env.rainImg = 0;
        g_env.snowImg = 0;
        g_weatherSpriteVer = ver;
    }
    if (!g_env.rainImg) {
        /* Tall thin streak: opaque core, soft edges — reads as rain when stretched. */
        const int w = 6, h = 48;
        std::vector<unsigned char> px((size_t)w * h * 4, 0);
        for (int y = 0; y < h; ++y) {
            float along = (float)y / (float)(h - 1);
            float tip = 1.f - std::fabs(along - 0.5f) * 1.6f;
            if (tip < 0.f) tip = 0.f;
            for (int x = 0; x < w; ++x) {
                float across = 1.f - std::fabs((x + 0.5f) / w - 0.5f) * 2.f;
                float a = tip * across * across;
                int alpha = (int)(a * 210.f);
                if (alpha < 1) continue;
                size_t p = ((size_t)y * w + x) * 4;
                px[p] = 200; px[p + 1] = 215; px[p + 2] = 235; px[p + 3] = (unsigned char)alpha;
            }
        }
        g_env.rainImg = env_make_rgba_image(w, h, px);
    }
    if (!g_env.snowImg) {
        /* Soft flake with a brighter center. */
        const int w = 16, h = 16;
        std::vector<unsigned char> px((size_t)w * h * 4, 0);
        float cx = 7.5f, cy = 7.5f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                if (d > 7.2f) continue;
                float a = 1.f - d / 7.2f;
                a = a * a;
                size_t p = ((size_t)y * w + x) * 4;
                px[p] = 250; px[p + 1] = 252; px[p + 2] = 255;
                px[p + 3] = (unsigned char)(30 + 200 * a);
            }
        }
        g_env.snowImg = env_make_rgba_image(w, h, px);
    }
}

static void env_weather_spawn(float dt) {
    if (g_env.weather == 0 || g_env.weatherIntensity <= 0) return;
    if (!g.render3d) return;
    env_ensure_weather_sprites();
    int img = g_env.weather == 2 ? g_env.snowImg : g_env.rainImg;
    if (!img) return;
    float inten = g_env.weatherIntensity / 100.f;
    float rate = inten * (g_env.weather == 2 ? 110.f : 220.f);
    g_env.weatherAcc += dt * rate;
    int guard = 32;
    int dim = 1;
    while (g_env.weatherAcc >= 1.f && guard-- > 0) {
        g_env.weatherAcc -= 1.f;
        if (part_slot() < 0) break;
        float rx = (part_rnd() - 0.5f) * 1100.f;
        float rz = (part_rnd() - 0.5f) * 1100.f;
        float ry = -280.f - part_rnd() * 120.f;
        float x = g_env.camSx + rx;
        float y = g_env.camSy + ry;
        float z = g_env.camSz + rz;
        if (g_env.weather == 2) {
            float size = 8.f + part_rnd() * 16.f;
            float wind = (part_rnd() - 0.5f) * 55.f;
            float drift = (part_rnd() - 0.5f) * 45.f;
            float fall = 25.f + part_rnd() * 35.f;
            float life = 2.8f + part_rnd() * 2.2f;
            int a = 160 + (int)(part_rnd() * 70.f);
            part_spawn_vel(img, -3, x, y, z, size, wind, fall, drift, life,
                           248, 250, 255, a, 1, dim, 0);
        } else {
            /* Near-vertical drop with a little wind; streak billboard does the look. */
            float size = 55.f + part_rnd() * 45.f;
            float wind = (part_rnd() - 0.5f) * 35.f;
            float fall = 520.f + part_rnd() * 280.f;
            float life = 0.55f + part_rnd() * 0.35f;
            int a = 140 + (int)(part_rnd() * 80.f);
            part_spawn_vel(img, -2, x, y, z, size, wind, fall, wind * 0.25f, life,
                           190, 205, 230, a, 0, dim, 1);
        }
    }
}

static void env_weather_lerp(float dt) {
    float targetWet = (g_env.weather == 1) ? env_clampf(g_env.weatherIntensity / 100.f, 0.f, 1.f) : 0.f;
    float targetSnow = (g_env.weather == 2) ? env_clampf(g_env.weatherIntensity / 100.f, 0.f, 1.f) : 0.f;
    float k = 1.f - std::exp(-dt * 0.7f);
    g_env.wetness += (targetWet - g_env.wetness) * k;
    g_env.snowCover += (targetSnow - g_env.snowCover) * k;
}

/* Tint mesh albedo for wet / snow ground look. */
static void env_tint_rgb(int& r, int& g, int& b) {
    if (g_env.wetness > 0.001f) {
        float w = g_env.wetness;
        r = (int)std::lround(env_lerp((float)r, (float)r * 0.45f + 20.f, w));
        g = (int)std::lround(env_lerp((float)g, (float)g * 0.50f + 30.f, w));
        b = (int)std::lround(env_lerp((float)b, (float)b * 0.55f + 55.f, w));
    }
    if (g_env.snowCover > 0.001f) {
        float s = g_env.snowCover;
        r = (int)std::lround(env_lerp((float)r, 235.f, s * 0.75f));
        g = (int)std::lround(env_lerp((float)g, 240.f, s * 0.75f));
        b = (int)std::lround(env_lerp((float)b, 248.f, s * 0.75f));
    }
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
}

static void env_tint_rgb_f(float& r, float& g, float& b) {
    int ir = (int)std::lround(r * 255.f);
    int ig = (int)std::lround(g * 255.f);
    int ib = (int)std::lround(b * 255.f);
    env_tint_rgb(ir, ig, ib);
    r = ir / 255.f; g = ig / 255.f; b = ib / 255.f;
}

static void env_reset() {
    g_env.dayCycle = 0;
    g_env.dayPaused = 0;
    g_env.dayTime = 0.35f;
    g_env.dayLength = 120.f;
    g_env.weather = 0;
    g_env.weatherIntensity = 50;
    g_env.wetness = 0.f;
    g_env.snowCover = 0.f;
    g_env.weatherAcc = 0.f;
    /* Keep procedural image ids; they live in g.images until cache clear. */
}

static void env_step(float dt) {
    if (dt < 0.f) dt = 0.f;
    if (dt > 0.05f) dt = 0.05f;
    env_weather_lerp(dt);
    if (g_env.dayCycle && !g_env.dayPaused && g_env.dayLength > 0.5f) {
        g_env.dayTime += dt / g_env.dayLength;
        while (g_env.dayTime >= 1.f) g_env.dayTime -= 1.f;
    }
    if (g_env.dayCycle) env_apply_day();
    env_weather_spawn(dt);
}

/* ---- C API ---- */

extern "C" int engine_day_cycle() { return g_env.dayCycle ? 1 : 0; }
extern "C" void engine_set_day_cycle(int on) {
    g_env.dayCycle = on ? 1 : 0;
    if (g_env.dayCycle) env_apply_day();
}
extern "C" float engine_day_time() { return g_env.dayTime; }
extern "C" void engine_set_day_time(float t) {
    g_env.dayTime = env_clampf(t, 0.f, 0.9999f);
    if (g_env.dayCycle) env_apply_day();
}
extern "C" float engine_day_length() { return g_env.dayLength; }
extern "C" void engine_set_day_length(float sec) {
    if (sec < 5.f) sec = 5.f;
    if (sec > 86400.f) sec = 86400.f;
    g_env.dayLength = sec;
}
extern "C" int engine_day_paused() { return g_env.dayPaused ? 1 : 0; }
extern "C" void engine_set_day_paused(int on) { g_env.dayPaused = on ? 1 : 0; }
extern "C" void engine_day_apply() { env_apply_day(); }

extern "C" int engine_weather() { return g_env.weather; }
extern "C" void engine_set_weather(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_env.weather = mode;
}
extern "C" int engine_weather_intensity() { return g_env.weatherIntensity; }
extern "C" void engine_set_weather_intensity(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_env.weatherIntensity = v;
}
extern "C" float engine_wetness() { return g_env.wetness; }
extern "C" float engine_snow_cover() { return g_env.snowCover; }
extern "C" void engine_set_wetness(float v) { g_env.wetness = env_clampf(v, 0.f, 1.f); }
extern "C" void engine_set_snow_cover(float v) { g_env.snowCover = env_clampf(v, 0.f, 1.f); }

extern "C" void engine_env_set_focus(float sx, float sy, float sz) {
    g_env.camSx = sx; g_env.camSy = sy; g_env.camSz = sz;
}

static void env_save_header(std::ostream& f) {
    if (g_env.dayCycle || g_env.dayTime != 0.35f || g_env.dayLength != 120.f || g_env.dayPaused)
        f << "DAYNIGHT " << (g_env.dayCycle ? 1 : 0) << " " << g_env.dayTime << " "
          << g_env.dayLength << " " << (g_env.dayPaused ? 1 : 0) << "\n";
    if (g_env.weather || g_env.weatherIntensity != 50)
        f << "WEATHER " << g_env.weather << " " << g_env.weatherIntensity << "\n";
}

static int env_try_load_line(const std::string& line) {
    if (line.rfind("DAYNIGHT ", 0) == 0) {
        std::istringstream ls(line.substr(9));
        int on = 0, paused = 0;
        float t = 0.35f, len = 120.f;
        ls >> on >> t >> len >> paused;
        g_env.dayCycle = on ? 1 : 0;
        g_env.dayTime = env_clampf(t, 0.f, 0.9999f);
        g_env.dayLength = len < 5.f ? 5.f : (len > 86400.f ? 86400.f : len);
        g_env.dayPaused = paused ? 1 : 0;
        if (g_env.dayCycle) env_apply_day();
        return 1;
    }
    if (line.rfind("WEATHER ", 0) == 0) {
        std::istringstream ls(line.substr(8));
        int mode = 0, inten = 50;
        ls >> mode >> inten;
        if (mode < 0) mode = 0;
        if (mode > 2) mode = 2;
        if (inten < 0) inten = 0;
        if (inten > 100) inten = 100;
        g_env.weather = mode;
        g_env.weatherIntensity = inten;
        return 1;
    }
    /* Ignore leftover SCATTER lines from older scenes. */
    if (line.rfind("SCATTER ", 0) == 0) return 1;
    return 0;
}
