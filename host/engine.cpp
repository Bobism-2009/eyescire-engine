// =============================================================================
// Eyescire Engine - implementation (Win32 + GDI). Compiled into engine.dll.
// See engine.h for the public C ABI consumed by editor.exe and script DLLs.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define EYESCIRE_BUILDING
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <dwmapi.h>

#include "engine.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <cstdlib>
#include <chrono>

namespace fs = std::filesystem;

static int  g_rawDx = 0, g_rawDy = 0;
static bool g_rawMouse = false;

static void raw_mouse_enable(HWND hwnd, bool on) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = on ? 0 : RIDEV_REMOVE;
    rid.hwndTarget = on ? hwnd : nullptr;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) g_rawMouse = on;
    else if (!on) g_rawMouse = false;
}

static void raw_mouse_msg(LPARAM lp) {
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) return;
    if (size == 0 || size > 256) return;
    BYTE buf[256];
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) return;
    RAWINPUT* raw = (RAWINPUT*)buf;
    if (raw->header.dwType != RIM_TYPEMOUSE) return;
    if (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;
    g_rawDx += raw->data.mouse.lLastX;
    g_rawDy += raw->data.mouse.lLastY;
}

static void raw_mouse_take(int& dx, int& dy) {
    dx = g_rawDx;
    dy = g_rawDy;
    g_rawDx = g_rawDy = 0;
}

namespace {

constexpr const char* kClassName = "EyescireEngineWindow";
constexpr int         kEntityMax  = 256;
constexpr int         kLogCap     = 512;
constexpr int         kTypedCap   = 64;

struct Image {
    Gdiplus::Bitmap* bmp = nullptr;   /* loaded via GDI+ so we get PNG/JPG + alpha */
    HBITMAP          hbmp = nullptr;  /* DIB copy for fast BitBlt scrolling */
    HDC              hdc  = nullptr;
    HGDIOBJ          old  = nullptr;
    /* 1bpp companion, bit set where the source is transparent, so a cut-out
       sprite can be blitted with the classic AND-mask/OR-colour pair.
       StretchBlt has no per-pixel alpha, and GetHBITMAP leaves transparent
       pixels black, so without this every rounded tile corner pastes a black
       notch over the sky. */
    HBITMAP          hmask   = nullptr;
    HDC              maskDC  = nullptr;
    HGDIOBJ          maskOld = nullptr;
    int              maskTried = 0;   /* mask build attempted (0 or 1) */
    int              hasAlpha  = 0;   /* source has transparent pixels */
    int w = 0;
    int h = 0;
};

static void image_release_dib(Image& img) {
    if (img.hdc) {
        if (img.old) SelectObject(img.hdc, img.old);
        DeleteDC(img.hdc);
        img.hdc = nullptr;
        img.old = nullptr;
    }
    if (img.hbmp) {
        DeleteObject(img.hbmp);
        img.hbmp = nullptr;
    }
    if (img.maskDC) {
        if (img.maskOld) SelectObject(img.maskDC, img.maskOld);
        DeleteDC(img.maskDC);
        img.maskDC = nullptr;
        img.maskOld = nullptr;
    }
    if (img.hmask) {
        DeleteObject(img.hmask);
        img.hmask = nullptr;
    }
    img.maskTried = 0;
    img.hasAlpha = 0;
}

static void image_ensure_dib(Image& img) {
    if (img.hdc && img.hbmp) return;
    if (!img.bmp) return;
    if (img.bmp->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &img.hbmp) != Gdiplus::Ok) return;
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    img.hdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!img.hdc) return;
    img.old = SelectObject(img.hdc, img.hbmp);
}

/* Build the transparency mask, once per image. Images with no transparent
   pixels get no mask and keep the cheaper single SRCCOPY path. */
static void image_ensure_mask(Image& img) {
    if (img.maskTried) return;
    img.maskTried = 1;
    if (!img.bmp) return;
    int w = (int)img.bmp->GetWidth();
    int h = (int)img.bmp->GetHeight();
    if (w <= 0 || h <= 0) return;

    Gdiplus::Rect rc(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (img.bmp->LockBits(&rc, Gdiplus::ImageLockModeRead,
                          PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return;

    /* CreateBitmap wants 1bpp rows padded out to a WORD, MSB leftmost. */
    int rowBytes = ((w + 15) / 16) * 2;
    std::vector<uint8_t> bits((size_t)rowBytes * (size_t)h, 0);
    int transparent = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = (const uint8_t*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        uint8_t* dst = &bits[(size_t)y * (size_t)rowBytes];
        for (int x = 0; x < w; ++x) {
            if (src[x * 4 + 3] >= 128) continue;   /* opaque: leave the bit clear */
            dst[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            ++transparent;
        }
    }
    img.bmp->UnlockBits(&bd);
    if (transparent == 0) return;

    img.hmask = CreateBitmap(w, h, 1, 1, bits.data());
    if (!img.hmask) return;
    HDC screen = GetDC(nullptr);
    if (!screen) { DeleteObject(img.hmask); img.hmask = nullptr; return; }
    img.maskDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!img.maskDC) { DeleteObject(img.hmask); img.hmask = nullptr; return; }
    img.maskOld = SelectObject(img.maskDC, img.hmask);
    img.hasAlpha = 1;
}


struct Entity {
    bool        active     = false;
    bool        selected   = false;
    int         id         = 0;
    int         parentSlot = -1;   /* -1 = root of hierarchy */
    bool        hierExpanded = true; /* editor: when false, children hidden in hierarchy */
    int         x          = 0;
    int         y          = 0;
    int         w          = 0;
    int         h          = 0;
    int         rotDeg     = 0;    /* roll around Z, degrees. 0..359 */
    int         rotX       = 0;    /* pitch around X, degrees. Scene fields 55 and 56. */
    int         rotY       = 0;    /* yaw around Y, degrees */
    float       lookYaw    = 0.f;  /* sub-degree look, used while lookFine */
    float       lookPitch  = 0.f;
    bool        lookFine   = false;
    bool        isCamera   = false; /* main scene camera (editor play mode) */
    /* 3D (scene v7): x/y stay the 2D/physics plane; z is how far into the
     * screen the entity starts, depth how far it extends (0 = kind default).
     * model is a .obj path for kind "model". */
    int         z          = 0;
    int         depth      = 0;
    std::string model;
    /* A sound that follows this object. Empty means silent. Loop keeps it
       going. Range is how far away it can still be heard, in scene pixels. */
    std::string sound;
    int         soundLoop  = 0;
    int         soundRange = 800;
    /* Emitter. The picture is sprite, tinted by r/g/b/a. Rate is particles
       per second. Life is milliseconds. Speed and size are scene pixels.
       Spread is degrees around up. HD is the 3D cloud: more particles, softer. */
    int         emitRate   = 20;
    int         emitLife   = 900;
    int         emitSpeed  = 80;
    int         emitSpread = 40;
    int         emitSize   = 28;
    int         emitHD     = 0;
    float       emitAcc    = 0;
    /* lights (scene v8), kinds "light_directional" / "light_point" /
     * "light_spot" / "light_ambient": color is r/g/b, the position is the
     * rect's center at z. Directional and spot lights shine down the screen,
     * turned by rotDeg and swung into the screen by lightTilt. */
    int         lightIntensity = 100;   /* percent */
    int         lightRange     = 400;   /* px, point and spot */
    int         lightAngle     = 45;    /* spot cone, degrees */
    int         lightTilt      = 0;     /* degrees into the screen */
    bool        visible    = true;  /* drawn when this and every parent are visible */
    /* Button colors. custom 0 = derived from the base color (darker on hover
       and press, lighter when toggled on). */
    struct BtnTint { int r = 0, g = 0, b = 0, custom = 0; };
    BtnTint     btnHover, btnPress, btnOn, btnOnHover;
    int         btnToggle  = 0;
    int         btnToggled = 0;
    /* Click action. btnScript is a scripts/ path such as "menu.nxa".
       btnFn is a no-argument function in that script. Scene fields 51 and 52. */
    std::string btnScript;
    std::string btnFn;
    int         r          = 220;
    int         g          = 220;
    int         b          = 220;
    int         a          = 255;   /* opacity; 255 is opaque. Scene field index 50. */
    std::string name;
    std::string kind;
    std::string text;
    std::string sprite;
    std::string script;
    int         spriteId   = 0;

    /* ---- physics (Box2D rigidbody component) ---- */
    bool        hasBody       = false;
    int         bodyType      = 1;      /* 0=static, 1=dynamic, 2=kinematic */
    float       density       = 1.0f;
    float       friction      = 0.3f;
    float       restitution   = 0.0f;
    float       gravityScale  = 1.0f;
    bool        fixedRotation = false;
    /* 0 = the rectangle (or circle). 1..5 = a mesh outline; higher follows the
       model more closely. Scene field 53. */
    int         hitbox        = 0;
    /* 0..100. Sliders and progress bars. Scene field 54. */
    int         uiValue       = 0;
    b2Body*     body          = nullptr;  /* runtime only, not serialized */
};

struct UndoEntry {
    int         op;          /* 0=modify, 1=create, 2=delete */
    int         slot;
    int         group;       /* 0 = its own step; shared id undoes together */
    Entity      snap;        /* full snapshot */
};

struct EngineState {
    HINSTANCE hinst    = nullptr;
    HWND      hwnd     = nullptr;
    HDC       backDC   = nullptr;
    HBITMAP   backBmp  = nullptr;
    HBITMAP   oldBackBmp = nullptr;
    HDC       frontDC  = nullptr;
    HBITMAP   frontBmp = nullptr;
    HGDIOBJ   oldFrontBmp = nullptr;
    int       frontW     = 0;
    int       frontH     = 0;
    HBRUSH    letterboxBrush = nullptr;
    HFONT     font     = nullptr;
    HFONT     oldFont  = nullptr;
    int       fontSize = 14;
    std::string fontFace = "Consolas";
    int       fontBold = 0;
    int       width    = 0;   /* render buffer width */
    int       height   = 0;   /* render buffer height */
    int       clientW  = 0;   /* window client area */
    int       clientH  = 0;
    int       displayX = 0;   /* scaled framebuffer placement */
    int       displayY = 0;
    int       displayW = 0;
    int       displayH = 0;
    double    displayScale = 1.0;
    bool      letterboxEnabled = false; /* false = stretch to fill window */
    bool      musicPlaying = false;
    int       musicVolume  = 750;       /* MCI volume 0..1000 */
    int       sfxVolume    = 1000;      /* 0..1000, used by future sfx playback */
    bool      autosaveEnabled = true;
    bool      fullscreen   = false;
    LONG      savedWindowStyle = 0;
    RECT      savedWindowRect{};
    int       settingsRes = 0;
    int       settingsFullscreen = 0;
    int       settingsMusic = 3;
    int       settingsSfx = 4;
    int       settingsParallax = 1;
    int       settingsAutosave = 1;
    int       settingsUiScale = 0;
    std::vector<std::string> worldNames;
    std::string              worldScratch;
    std::vector<std::string> playerNames;
    std::string              playerScratch;
    struct PlayerLook {
        int hair = 0;
        int shirt = 0;
        int pants = 0;
        int hat = 0;
        int acc = 0;
        int skin_rgb = 0xFFA276;
        int hair_rgb = 0x9C4126;
        int shirt_rgb = 0x37AB4C;
        int pants_rgb = 0xFF8C23;
        int hat_rgb = 0xDCB430;
        int acc_rgb = 0xA86C34;
        int undershirt_rgb = 0xA0B4D7;
        int shoes_rgb = 0x322A28;
    };
    std::vector<PlayerLook> playerLooks;
    bool      quit     = false;

    int  mouseX = 0;
    int  mouseY = 0;
    bool mouseDown[3]     = { false, false, false };
    bool mousePressed[3]  = { false, false, false };
    bool mouseReleased[3] = { false, false, false };
    int  mouseWheelAccum  = 0;

    bool keyDown[256]     = { false };
    bool keyPressed[256]  = { false };
    bool keyReleased[256] = { false };

    int  typed[kTypedCap] = { 0 };
    int  typedHead = 0;
    int  typedTail = 0;

    std::unordered_map<int, Image> images;
    std::unordered_map<std::string, int> imageByPath; /* cache for thumbnails */
    int nextImageId = 1;
    ULONG_PTR gdiplusToken = 0;

    LARGE_INTEGER perfFreq{};
    LARGE_INTEGER perfStart{};
    double        dtSeconds = 0.0;

    std::vector<Entity> entities;   /* size = kEntityMax */
    int                 nextEntityId = 1;
    int                 selfSlot     = -1;

    /* ---- physics (Box2D) ---- */
    b2World* physWorld   = nullptr;
    float    gravityX    = 0.0f;      /* pixels / s^2 */
    float    gravityY    = 980.0f;    /* down (screen +y) */
    bool     render3d    = false;     /* scene RENDER line; see engine_scene_render3d */
    float    pixelsPerMeter = 50.0f;
    int      clearR = 28, clearG = 32, clearB = 40;
    float    sceneFov = 60.0f;
    int      skyMode = 4;             /* 0 day, 1 sunset, 2 night, 3 custom, 4 solid */
    std::string skyFace[6];           /* right left up down front back */

    std::vector<std::string> log;
    std::string              lastCmdOutput;

    std::unordered_map<int, HMODULE> dlls;
    std::unordered_map<std::string, int> dllByPath; /* normalized path -> handle */
    int nextDllHandle = 1;

    /* undo/redo */
    std::vector<UndoEntry>   undoStack;
    std::vector<UndoEntry>   redoStack;
    int                      undoGroup = 0;     /* 0 = each save is its own step */
    int                      undoGroupGen = 1;

    /* text editor buffer (single shared) */
    std::vector<std::string> teLines;
    int                      teCursorLine = 0;
    int                      teCursorCol  = 0;
    int                      teScrollY    = 0;
    bool                     teDirty      = false;
    std::string              tePath;

    /* project state */
    bool                     projLoaded = false;
    std::string              projRoot;
    std::string              projName;
    std::string              projStart;    /* scene opened with the project */
    std::string              projManifest;
    std::unordered_map<std::string, int> prefs;
    std::vector<std::string> projScenes;     /* "scenes/main.scene" or "scenes/levels/1.scene" */
    int                      projActiveScene = 0;
    std::vector<std::string> projScripts;    /* under scripts/: "player.nxa" or "ui/hud.nxa" */
    std::vector<std::string> projBlueprints; /* under scripts/: "mover.ebp" or "ui/mover.ebp" */
    std::vector<std::string> projPrefabs;    /* under prefabs/: "coin.prefab" or "items/coin.prefab" */
    std::vector<std::string> projAssets;     /* under assets/: "player.png" or "ui/button.png" */
    std::vector<std::string> projRecent;
    std::string              projRecentFile;
    std::string              projTmp;        /* return scratch for path_join etc. */

    /* camera view (editor / runtime / scripts) */
    float viewCamX = 0.0f;
    float viewCamY = 0.0f;
    float viewZoom = 1.0f;
    int   viewX = 0;
    int   viewY = 0;
    int   viewW = 0;
    int   viewH = 0;
};

struct TileSheetFrame {
    int sx = 0;
    int sy = 0;
    int sw = 16;
    int sh = 16;
    /* 2x2 gapped autotile pieces assemble into one 32px world tile so the
       sheet matches dirt and the camera zoom instead of stretching a 16px
       corner across the whole cell. */
    int tilesX = 1;
    int tilesY = 1;
    int stride = 0;
    /* Whether this particular frame is free of transparent pixels. Most of an
       autotile sheet is interior frames that are fully opaque, and those are
       also the ones that cover most of the screen underground. Blitting them
       through the transparency mask costs a second, colour-converting pass
       for nothing. */
    int opaque = 0;
};

struct TileDef {
    int r = 0, g = 0, b = 0;
    int r2 = 0, g2 = 0, b2 = 0;
    int solid = 0;
    std::string sprite;
    int spriteId = 0;
    /* Alternative sprites for the same tile id, picked per cell at draw time.
       One texture repeated every tile reads as an obvious grid; a handful of
       variants breaks that up without touching the map data. */
    std::vector<int> variantIds;
    int sheetId = 0;
    int sheetCell = 16;
    int sheetStride = 16;
    std::vector<TileSheetFrame> sheetFrames;
    /* Autotile frames bucketed by which of a cell's four sides has no
       merging neighbour: bit0 up, bit1 down, bit2 left, bit3 right. Picking a
       frame by context rather than at random is what makes a run of grass
       grow a crown along its top and cap off at its ends, instead of every
       cell independently guessing which way it faces. */
    std::vector<TileSheetFrame> autoFrames[16];
    int autotile = 0;
    /* Bit per mask whose bucket has been set explicitly, so the first
       explicit frame for a mask replaces whatever was derived. */
    int autoOverride = 0;
};

struct TilemapState {
    int w = 0;
    int h = 0;
    int tile = 16;
    std::vector<uint8_t> cells;
    TileDef defs[256];
    int skyR = 92, skyG = 168, skyB = 228;
    int cursorTx = -1, cursorTy = -1;
    int lastX = 0, lastY = 0, lastHitX = 0, lastHitY = 0, lastGrounded = 0;
};

/* A parallax backdrop layer drawn behind the tiles. Kept with the tilemap
   because that is what decides where the horizon is. */
struct BackdropLayer {
    int imageId = 0;
    int par = 1000;      /* horizontal parallax, per-mille of camera movement */
    int anchorY = 0;     /* world Y the layer's bottom sits on (anchored mode) */
    int scale = 1000;    /* per-mille of native size, in world px */
    int fillView = 0;    /* 1 = stretch to the whole view, in screen space */
};

EngineState g;
TilemapState g_tiles;
std::vector<BackdropLayer> g_backdrop;
static int g_tilemap_draw_hidden = 0;

/* Used as a return-only buffer for engine_fs_list so the caller can
 * cache the pointer until the next call. */
std::string g_fsListEntry;

/* ----- helpers ---------------------------------------------------------- */

/* Fonts are cached by face/size/weight: UI code switches size and face many
 * times per frame, and CreateFontA on every switch is the dominant cost. The
 * set of combinations is small, so entries live until engine_shutdown. */
static std::unordered_map<std::string, HFONT> g_fontCache;

/* One anti-aliased GDI+ context on the back buffer, shared by the rounded
 * primitives (engine_fill_round_a / engine_outline_round_a): editor chrome
 * draws dozens a frame, and constructing a Graphics per call dominated frame
 * time. GDI+ takes the DC's clip region when the context is built, so it is
 * dropped whenever the clip or the back buffer changes, and at present. Each
 * draw flushes so later GDI calls (TextOut) land on top in call order. */
static Gdiplus::Graphics* g_roundGfx = nullptr;

static void round_gfx_drop() {
    if (g_roundGfx) {
        delete g_roundGfx;
        g_roundGfx = nullptr;
    }
}

static Gdiplus::Graphics* round_gfx() {
    if (!g_roundGfx && g.backDC) {
        g_roundGfx = new Gdiplus::Graphics(g.backDC);
        g_roundGfx->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    }
    return g_roundGfx;
}

void rebuild_font() {
    const std::string face = g.fontFace.empty() ? std::string("Consolas") : g.fontFace;
    const std::string key = face + "|" + std::to_string(g.fontSize) + "|" + (g.fontBold ? "b" : "n");
    HFONT f = nullptr;
    auto it = g_fontCache.find(key);
    if (it != g_fontCache.end()) {
        f = it->second;
    } else {
        f = CreateFontA(
            -g.fontSize, 0, 0, 0,
            g.fontBold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            FF_DONTCARE,
            face.c_str());
        if (f) g_fontCache[key] = f;
    }
    g.font = f;
    if (g.backDC && g.font) {
        HFONT prev = (HFONT)SelectObject(g.backDC, g.font);
        if (!g.oldFont) g.oldFont = prev;   /* the DC's original font, restored at shutdown */
        SetBkMode(g.backDC, TRANSPARENT);
    }
}

static void release_fonts() {
    if (g.backDC && g.oldFont) SelectObject(g.backDC, g.oldFont);
    g.oldFont = nullptr;
    for (auto& kv : g_fontCache) DeleteObject(kv.second);
    g_fontCache.clear();
    g.font = nullptr;
}

void log_push(std::string msg) {
    if (g.log.size() >= (size_t)kLogCap) {
        g.log.erase(g.log.begin());
    }
    g.log.emplace_back(std::move(msg));
}

bool valid_slot(int s) {
    return s >= 0 && s < (int)g.entities.size();
}

bool active_slot(int s) {
    return valid_slot(s) && g.entities[s].active;
}

/* False when this entity or any ancestor is hidden. */
bool entity_shown(int slot) {
    int guard = 0;
    while (active_slot(slot) && guard < 64) {
        if (!g.entities[slot].visible) return false;
        slot = g.entities[slot].parentSlot;
        ++guard;
    }
    return true;
}

static void update_display_layout() {
    if (!g.hwnd) return;
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    g.clientW = rc.right - rc.left;
    g.clientH = rc.bottom - rc.top;
    if (g.clientW < 1) g.clientW = 1;
    if (g.clientH < 1) g.clientH = 1;
    if (g.width < 1 || g.height < 1) return;

    const double scaleX = (double)g.clientW / (double)g.width;
    const double scaleY = (double)g.clientH / (double)g.height;
    if (g.letterboxEnabled) {
        g.displayScale = scaleX < scaleY ? scaleX : scaleY;
        g.displayW = (int)(g.width * g.displayScale);
        g.displayH = (int)(g.height * g.displayScale);
        if (g.displayW < 1) g.displayW = 1;
        if (g.displayH < 1) g.displayH = 1;
        g.displayX = (g.clientW - g.displayW) / 2;
        g.displayY = (g.clientH - g.displayH) / 2;
    } else {
        g.displayScale = 0.0;
        g.displayW = g.clientW;
        g.displayH = g.clientH;
        g.displayX = 0;
        g.displayY = 0;
    }
}

static void map_client_mouse(int cx, int cy) {
    int bx = 0;
    int by = 0;
    if (g.letterboxEnabled) {
        if (g.displayScale <= 0.0) {
            g.mouseX = cx;
            g.mouseY = cy;
            return;
        }
        bx = (int)((cx - g.displayX) / g.displayScale);
        by = (int)((cy - g.displayY) / g.displayScale);
    } else {
        if (g.clientW < 1) g.clientW = 1;
        if (g.clientH < 1) g.clientH = 1;
        bx = (int)((int64_t)cx * g.width / g.clientW);
        by = (int)((int64_t)cy * g.height / g.clientH);
    }
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (bx >= g.width) bx = g.width - 1;
    if (by >= g.height) by = g.height - 1;
    g.mouseX = bx;
    g.mouseY = by;
}

static void release_front_buffer() {
    if (g.frontDC && g.oldFrontBmp) SelectObject(g.frontDC, g.oldFrontBmp);
    if (g.frontBmp) { DeleteObject(g.frontBmp); g.frontBmp = nullptr; }
    if (g.frontDC) { DeleteDC(g.frontDC); g.frontDC = nullptr; }
    g.oldFrontBmp = nullptr;
    g.frontW = 0;
    g.frontH = 0;
}

static void ensure_front_buffer() {
    update_display_layout();
    if (g.frontDC && g.frontW == g.clientW && g.frontH == g.clientH) return;
    release_front_buffer();
    if (!g.hwnd || g.clientW < 1 || g.clientH < 1) return;

    HDC screen = GetDC(g.hwnd);
    if (!screen) return;
    g.frontDC = CreateCompatibleDC(screen);
    g.frontBmp = CreateCompatibleBitmap(screen, g.clientW, g.clientH);
    ReleaseDC(g.hwnd, screen);
    if (!g.frontDC || !g.frontBmp) {
        release_front_buffer();
        return;
    }
    g.oldFrontBmp = SelectObject(g.frontDC, g.frontBmp);
    g.frontW = g.clientW;
    g.frontH = g.clientH;
}

static void compose_frame() {
    if (!g.backDC || g.width <= 0 || g.height <= 0) return;
    ensure_front_buffer();
    if (!g.frontDC) return;

    SetStretchBltMode(g.frontDC, COLORONCOLOR);
    if (g.letterboxEnabled) {
        if (!g.letterboxBrush) g.letterboxBrush = CreateSolidBrush(RGB(0, 0, 0));
        RECT fill{ 0, 0, g.frontW, g.frontH };
        FillRect(g.frontDC, &fill, g.letterboxBrush);
        StretchBlt(g.frontDC, g.displayX, g.displayY, g.displayW, g.displayH,
                   g.backDC, 0, 0, g.width, g.height, SRCCOPY);
    } else {
        if (g.clientW == g.width && g.clientH == g.height)
            BitBlt(g.frontDC, 0, 0, g.clientW, g.clientH, g.backDC, 0, 0, SRCCOPY);
        else
            StretchBlt(g.frontDC, 0, 0, g.clientW, g.clientH,
                       g.backDC, 0, 0, g.width, g.height, SRCCOPY);
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g.quit = true;
            return 0;

        case WM_SIZE:
            release_front_buffer();
            update_display_layout();
            return 0;

        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONDOWN:
            map_client_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            g.mouseDown[0]    = true;
            g.mousePressed[0] = true;
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            map_client_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            g.mouseDown[0]     = false;
            g.mouseReleased[0] = true;
            if (!g.mouseDown[1] && !g.mouseDown[2]) ReleaseCapture();
            return 0;
        case WM_RBUTTONDOWN:
            map_client_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            g.mouseDown[1]    = true;
            g.mousePressed[1] = true;
            return 0;
        case WM_RBUTTONUP:
            map_client_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            g.mouseDown[1]     = false;
            g.mouseReleased[1] = true;
            return 0;
        case WM_MBUTTONDOWN:
            map_client_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            g.mouseDown[2]    = true;
            g.mousePressed[2] = true;
            return 0;
        case WM_MBUTTONUP:
            map_client_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            g.mouseDown[2]     = false;
            g.mouseReleased[2] = true;
            return 0;
        case WM_MOUSEMOVE:
            map_client_mouse((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            return 0;
        case WM_MOUSEWHEEL:
            g.mouseWheelAccum += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            return 0;
        case WM_INPUT:
            raw_mouse_msg(lp);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int vk = (int)wp;
            if (vk >= 0 && vk < 256) {
                if (!g.keyDown[vk]) g.keyPressed[vk] = true;
                g.keyDown[vk] = true;
            }
            if (vk == VK_F4 && (lp & (1 << 29))) {
                g.quit = true;
            }
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const int vk = (int)wp;
            if (vk >= 0 && vk < 256) {
                g.keyDown[vk]     = false;
                g.keyReleased[vk] = true;
            }
            return 0;
        }
        case WM_KILLFOCUS:
            /* key-ups sent after focus leaves (Alt+Tab's Alt, say) go to the other
               window; release everything so nothing stays held down */
            for (int vk = 0; vk < 256; ++vk) {
                if (g.keyDown[vk]) { g.keyDown[vk] = false; g.keyReleased[vk] = true; }
            }
            for (int b = 0; b < 3; ++b) {
                if (g.mouseDown[b]) { g.mouseDown[b] = false; g.mouseReleased[b] = true; }
            }
            engine_mouse_lock(0);
            return 0;
        case WM_CHAR: {
            int next = (g.typedTail + 1) % kTypedCap;
            if (next != g.typedHead) {
                g.typed[g.typedTail] = (int)wp;
                g.typedTail = next;
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (g.frontDC) {
                BitBlt(hdc, 0, 0, g.clientW, g.clientH, g.frontDC, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

}  // anonymous namespace

/* ============================================================ */
/*  Lifecycle                                                   */
/* ============================================================ */

extern "C" const char* engine_exe_dir();

static HICON g_windowIconBig = nullptr;
static HICON g_windowIconSmall = nullptr;

/* 32-bit icon with a real alpha channel, from the Eyescire mark PNG. */
static HICON icon_from_file(const std::string& path, int size) {
    if (size <= 0) return nullptr;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wpath((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

    Gdiplus::Bitmap src(wpath.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return nullptr;

    Gdiplus::Bitmap scaled(size, size, PixelFormat32bppPARGB);
    Gdiplus::Graphics gfx(&scaled);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    gfx.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    gfx.Clear(Gdiplus::Color(0, 0, 0, 0));
    gfx.DrawImage(&src, 0, 0, size, size);

    Gdiplus::Rect rc(0, 0, size, size);
    Gdiplus::BitmapData bd{};
    if (scaled.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok)
        return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        scaled.UnlockBits(&bd);
        return nullptr;
    }
    for (int y = 0; y < size; ++y) {
        std::memcpy((unsigned char*)bits + (size_t)y * (size_t)size * 4,
                    (unsigned char*)bd.Scan0 + (ptrdiff_t)y * bd.Stride,
                    (size_t)size * 4);
    }
    scaled.UnlockBits(&bd);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

static void window_icon_load() {
    std::string path = std::string(engine_exe_dir()) + "\\icons\\eyescire.png";
    int big = GetSystemMetrics(SM_CXICON);
    int small = GetSystemMetrics(SM_CXSMICON);
    if (big < 16) big = 32;
    if (small < 16) small = 16;
    g_windowIconBig = icon_from_file(path, big);
    g_windowIconSmall = icon_from_file(path, small);
}

static void window_icon_free() {
    if (g_windowIconBig) { DestroyIcon(g_windowIconBig); g_windowIconBig = nullptr; }
    if (g_windowIconSmall) { DestroyIcon(g_windowIconSmall); g_windowIconSmall = nullptr; }
}



/* Engine state without a window, for hosts that draw with something else (the
 * gfx3d player): entities, scenes, images, scripts, physics and timing all
 * work; engine drawing calls are no-ops (no back buffer); input comes from
 * engine_input_* instead of window messages. */
extern "C" int engine_init_headless(int w, int h) {
    if (g.gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput gpsi;
        Gdiplus::GdiplusStartup(&g.gdiplusToken, &gpsi, nullptr);
    }
    g.hinst  = GetModuleHandleA(nullptr);
    g.width  = w;
    g.height = h;
    if (g.entities.empty()) g.entities.assign(kEntityMax, Entity{});
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g.perfFreq);
    QueryPerformanceCounter(&g.perfStart);
    return 1;
}

/* ----- input fed by a host that owns its own window -------------------------- */

/* Start of a frame: clear the one-frame edges, as engine_pump does. */
extern "C" void engine_input_begin_frame() {
    for (int i = 0; i < 3; ++i) { g.mousePressed[i] = false; g.mouseReleased[i] = false; }
    for (int i = 0; i < 256; ++i) { g.keyPressed[i] = false; g.keyReleased[i] = false; }
    g.mouseWheelAccum = 0;
}
extern "C" void engine_input_key(int vk, int down) {
    if (vk < 0 || vk >= 256) return;
    if (down && !g.keyDown[vk]) g.keyPressed[vk] = true;
    if (!down && g.keyDown[vk]) g.keyReleased[vk] = true;
    g.keyDown[vk] = down != 0;
}
extern "C" void engine_input_mouse(int x, int y) { g.mouseX = x; g.mouseY = y; }
extern "C" void engine_input_button(int b, int down) {
    if (b < 0 || b >= 3) return;
    if (down && !g.mouseDown[b]) g.mousePressed[b] = true;
    if (!down && g.mouseDown[b]) g.mouseReleased[b] = true;
    g.mouseDown[b] = down != 0;
}
extern "C" void engine_input_wheel(int notches) { g.mouseWheelAccum += notches; }

/* Start a program and return without waiting (unlike engine_run_command).
 * 1 if it started. */
extern "C" int engine_launch(const char* cmd_line, const char* cwd) {
    if (!cmd_line || !*cmd_line) return 0;
    std::string cmd = cmd_line;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             (cwd && *cwd) ? cwd : nullptr, &si, &pi);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* A requested client size bigger than the desktop is shrunk to fit, keeping
   the aspect ratio, so a 3840x2160 play size cannot open past the monitor. */
static void clamp_window_to_work(int& w, int& h) {
    RECT work{};
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) return;
    int maxW = work.right - work.left;
    int maxH = work.bottom - work.top;
    if (maxW < 640 || maxH < 360) return;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int fw = rc.right - rc.left;
    int fh = rc.bottom - rc.top;
    if (fw <= maxW && fh <= maxH) return;
    double sx = (double)maxW / (double)fw;
    double sy = (double)maxH / (double)fh;
    double s = sx < sy ? sx : sy;
    w = (int)(w * s);
    h = (int)(h * s);
    if (w < 640) w = 640;
    if (h < 360) h = 360;
}

extern "C" int engine_init(int w, int h, const char* title) {
    if (g.hwnd) return 1;
    clamp_window_to_work(w, h);

    /* boot GDI+ once -- needed for PNG/JPG image loading */
    if (g.gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput gpsi;
        Gdiplus::GdiplusStartup(&g.gdiplusToken, &gpsi, nullptr);
    }

    g.hinst   = GetModuleHandleA(nullptr);
    g.width   = w;
    g.height  = h;
    g.entities.assign(kEntityMax, Entity{});

    window_icon_load();

    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = g.hinst;
    wc.lpszClassName = kClassName;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon         = g_windowIconBig;
    wc.hIconSm       = g_windowIconSmall;
    wc.style         = CS_OWNDC;
    RegisterClassExA(&wc);

    RECT rc{ 0, 0, w, h };
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    AdjustWindowRect(&rc, style, FALSE);

    g.hwnd = CreateWindowA(
        kClassName, title ? title : "Eyescire", style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, g.hinst, nullptr);
    if (!g.hwnd) {
        window_icon_free();
        return 0;
    }

    HDC windowDC = GetDC(g.hwnd);
    g.backDC      = CreateCompatibleDC(windowDC);
    g.backBmp     = CreateCompatibleBitmap(windowDC, w, h);
    g.oldBackBmp  = (HBITMAP)SelectObject(g.backDC, g.backBmp);
    ReleaseDC(g.hwnd, windowDC);

    rebuild_font();

    /* 1 ms scheduler ticks while we run: the frame limiter sleeps off each
     * frame's remainder, and at the default ~15.6 ms granularity a 60 fps
     * target lands near 40 fps. Paired with timeEndPeriod in engine_shutdown. */
    timeBeginPeriod(1);

    QueryPerformanceFrequency(&g.perfFreq);
    QueryPerformanceCounter(&g.perfStart);

    HBRUSH br = CreateSolidBrush(RGB(24, 26, 32));
    RECT frc{ 0, 0, g.width, g.height };
    FillRect(g.backDC, &frc, br);
    DeleteObject(br);

    update_display_layout();
    return 1;
}

extern "C" void engine_tilemap_destroy();

extern "C" void engine_music_stop();
static void sfx_tick();
static void part_tick();
static void part_reset();
static void sfx_on_scene_loaded();
static void sfx_shutdown();

extern "C" void engine_shutdown() {
    engine_music_stop();
    sfx_shutdown();
    engine_tilemap_destroy();
    engine_physics_end();

    for (auto& kv : g.dlls) {
        if (kv.second) FreeLibrary(kv.second);
    }
    g.dlls.clear();
    g.dllByPath.clear();

    for (auto& kv : g.images) {
        delete kv.second.bmp;
    }
    g.images.clear();

    round_gfx_drop();
    timeEndPeriod(1);
    if (g.gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(g.gdiplusToken);
        g.gdiplusToken = 0;
    }

    release_fonts();
    if (g.backDC && g.oldBackBmp) {
        SelectObject(g.backDC, g.oldBackBmp);
        g.oldBackBmp = nullptr;
    }
    if (g.backBmp) { DeleteObject(g.backBmp); g.backBmp = nullptr; }
    if (g.backDC)  { DeleteDC(g.backDC);     g.backDC  = nullptr; }
    release_front_buffer();
    if (g.letterboxBrush) {
        DeleteObject(g.letterboxBrush);
        g.letterboxBrush = nullptr;
    }
    if (g.hwnd)    { DestroyWindow(g.hwnd);  g.hwnd    = nullptr; }
    window_icon_free();
}

extern "C" int engine_pump() {
    for (int i = 0; i < 3; ++i) {
        g.mousePressed[i]  = false;
        g.mouseReleased[i] = false;
    }
    g.mouseWheelAccum = 0;
    for (int i = 0; i < 256; ++i) {
        g.keyPressed[i]  = false;
        g.keyReleased[i] = false;
    }
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g.quit) break;
    }
    return g.quit ? 0 : 1;
}

static int g_viewHole = 0, g_viewX = 0, g_viewY = 0, g_viewW = 0, g_viewH = 0;

static void view_hole(int on, int x, int y, int w, int h) {
    g_viewHole = on;
    g_viewX = x;
    g_viewY = y;
    g_viewW = w;
    g_viewH = h;
}

namespace r3d { void overlay_present(); }

/* 1 waits for the monitor. The 3D view uses the GL swap interval. A 2D frame
   has no GL swap, so present waits with DwmFlush instead. */
static int g_vsync = 1;
static int g_hd = 0;          /* 1: the 3D picture is drawn at twice the resolution */
static int g_glPresented = 0;

extern "C" int engine_vsync() { return g_vsync ? 1 : 0; }

extern "C" void engine_present() {
    if (!g.hwnd || !g.backDC) return;
    sfx_tick();
    part_tick();
    r3d::overlay_present();
    int glWaited = g_glPresented;
    g_glPresented = 0;
    round_gfx_drop();
    compose_frame();
    if (!g.frontDC) return;
    HDC windowDC = GetDC(g.hwnd);
    const bool hole = g_viewHole && g.clientW == g.width && g.clientH == g.height
                      && g_viewW > 0 && g_viewH > 0;
    if (!hole) {
        BitBlt(windowDC, 0, 0, g.clientW, g.clientH, g.frontDC, 0, 0, SRCCOPY);
    } else {
        /* Leave the 3D child window uncovered. Paint the chrome around it. */
        int x = g_viewX, y = g_viewY, w = g_viewW, h = g_viewH;
        if (y > 0)
            BitBlt(windowDC, 0, 0, g.clientW, y, g.frontDC, 0, 0, SRCCOPY);
        if (y + h < g.clientH)
            BitBlt(windowDC, 0, y + h, g.clientW, g.clientH - (y + h), g.frontDC, 0, y + h, SRCCOPY);
        if (x > 0)
            BitBlt(windowDC, 0, y, x, h, g.frontDC, 0, y, SRCCOPY);
        if (x + w < g.clientW)
            BitBlt(windowDC, x + w, y, g.clientW - (x + w), h, g.frontDC, x + w, y, SRCCOPY);
    }
    ReleaseDC(g.hwnd, windowDC);
    if (g_vsync && !glWaited) DwmFlush();
}

extern "C" int  engine_window_w() { return g.width; }
extern "C" int  engine_window_h() { return g.height; }
extern "C" void engine_set_title(const char* title) {
    if (g.hwnd && title) SetWindowTextA(g.hwnd, title);
}

static void resize_back_buffer(int w, int h) {
    if (!g.hwnd || !g.backDC || w < 320 || h < 240) return;
    round_gfx_drop();
    g.width = w;
    g.height = h;
    if (g.oldBackBmp) SelectObject(g.backDC, g.oldBackBmp);
    if (g.backBmp) { DeleteObject(g.backBmp); g.backBmp = nullptr; }
    HDC hdc = GetDC(g.hwnd);
    g.backBmp = CreateCompatibleBitmap(hdc, w, h);
    g.oldBackBmp = (HBITMAP)SelectObject(g.backDC, g.backBmp);
    ReleaseDC(g.hwnd, hdc);
    release_front_buffer();
}

extern "C" int engine_resize(int w, int h) {
    if (!g.hwnd || w < 320 || h < 240) return 0;
    if (!g.fullscreen) clamp_window_to_work(w, h);
    if (g.width == w && g.height == h) return 1;
    if (g.fullscreen) {
        resize_back_buffer(w, h);
        update_display_layout();
        return 1;
    }
    resize_back_buffer(w, h);
    DWORD style = (DWORD)GetWindowLong(g.hwnd, GWL_STYLE);
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, style, FALSE);
    SetWindowPos(g.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    update_display_layout();
    return 1;
}

/* Window client size in pixels, and a back buffer resize that leaves the window
   alone: a host that lays itself out to the window (the editor) calls
   engine_fit_buffer(client w, h) so nothing is stretched. */
/* Mouse lock, for look-around cameras: the cursor is hidden and held where it
   was, and engine_mouse_lock_poll() (once a frame) collects how far it tried to
   move, so a drag is never stopped by the window or screen edge. Unlocking
   shows the cursor again at the same spot. */
static bool  g_mouseLocked = false;
static POINT g_mouseLockAt{};
static int   g_mouseLockDx = 0, g_mouseLockDy = 0;

extern "C" void engine_mouse_lock(int on) {
    const bool want = on != 0;
    if (want == g_mouseLocked) return;
    g_mouseLocked = want;
    g_mouseLockDx = g_mouseLockDy = 0;
    g_rawDx = g_rawDy = 0;
    if (want) {
        GetCursorPos(&g_mouseLockAt);
        if (g.hwnd) {
            SetCapture(g.hwnd);
            raw_mouse_enable(g.hwnd, true);
        }
        ShowCursor(FALSE);
    } else {
        raw_mouse_enable(g.hwnd, false);
        SetCursorPos(g_mouseLockAt.x, g_mouseLockAt.y);
        ShowCursor(TRUE);
        if (!g.mouseDown[0]) ReleaseCapture();
    }
}
extern "C" int engine_mouse_locked() { return g_mouseLocked ? 1 : 0; }
extern "C" void engine_mouse_lock_poll() {
    g_mouseLockDx = g_mouseLockDy = 0;
    if (!g_mouseLocked) return;
    if (g_rawMouse) {
        raw_mouse_take(g_mouseLockDx, g_mouseLockDy);
        POINT p{};
        if (GetCursorPos(&p) && (p.x != g_mouseLockAt.x || p.y != g_mouseLockAt.y))
            SetCursorPos(g_mouseLockAt.x, g_mouseLockAt.y);
        return;
    }
    POINT p{};
    if (!GetCursorPos(&p)) return;
    g_mouseLockDx = p.x - g_mouseLockAt.x;
    g_mouseLockDy = p.y - g_mouseLockAt.y;
    if (g_mouseLockDx != 0 || g_mouseLockDy != 0) SetCursorPos(g_mouseLockAt.x, g_mouseLockAt.y);
}
extern "C" int engine_mouse_lock_dx() { return g_mouseLockDx; }
extern "C" int engine_mouse_lock_dy() { return g_mouseLockDy; }

extern "C" int engine_client_w() { return g.clientW; }
extern "C" int engine_client_h() { return g.clientH; }
extern "C" int engine_fit_buffer(int w, int h) {
    if (!g.hwnd || w < 320 || h < 240) return 0;
    if (g.width == w && g.height == h) return 1;
    resize_back_buffer(w, h);
    update_display_layout();
    return 1;
}

extern "C" void engine_set_fullscreen(int on) {
    if (!g.hwnd) return;
    const bool want = (on != 0);
    if (want == g.fullscreen) return;

    if (want) {
        g.savedWindowStyle = GetWindowLong(g.hwnd, GWL_STYLE);
        GetWindowRect(g.hwnd, &g.savedWindowRect);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLong(g.hwnd, GWL_STYLE,
                      g.savedWindowStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                                             WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
        SetWindowPos(g.hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g.fullscreen = true;
    } else {
        SetWindowLong(g.hwnd, GWL_STYLE, g.savedWindowStyle);
        SetWindowPos(g.hwnd, nullptr,
                     g.savedWindowRect.left, g.savedWindowRect.top,
                     g.savedWindowRect.right - g.savedWindowRect.left,
                     g.savedWindowRect.bottom - g.savedWindowRect.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g.fullscreen = false;
    }
    release_front_buffer();
    update_display_layout();
}

extern "C" int engine_fullscreen() {
    return g.fullscreen ? 1 : 0;
}

extern "C" void engine_set_letterbox(int on) {
    g.letterboxEnabled = (on != 0);
    release_front_buffer();
    update_display_layout();
}

extern "C" int engine_letterbox() {
    return g.letterboxEnabled ? 1 : 0;
}

extern "C" void engine_enable_letterbox() {
    engine_set_letterbox(1);
}

extern "C" void engine_disable_letterbox() {
    engine_set_letterbox(0);
}

/* ============================================================ */
/*  Drawing                                                     */
/* ============================================================ */

static int g_frameClearR = 21, g_frameClearG = 22, g_frameClearB = 25;

/* The editor fills and strokes hundreds of rectangles a frame. Creating and
   destroying a GDI brush or pen per call was most of that cost. The stock
   DC brush and pen take their color from the DC, so nothing is allocated. */
static HBRUSH fill_brush(int r, int g_, int b) {
    SetDCBrushColor(g.backDC, RGB(r, g_, b));
    return (HBRUSH)GetStockObject(DC_BRUSH);
}

extern "C" void engine_clear(int r, int g_, int b) {
    g_frameClearR = r;
    g_frameClearG = g_;
    g_frameClearB = b;
    if (!g.backDC) return;
    RECT rc{ 0, 0, g.width, g.height };
    FillRect(g.backDC, &rc, fill_brush(r, g_, b));
}

extern "C" void engine_fill_rect(int x, int y, int w, int h, int r, int g_, int b) {
    if (!g.backDC) return;
    RECT rc{ x, y, x + w, y + h };
    FillRect(g.backDC, &rc, fill_brush(r, g_, b));
}

extern "C" void engine_outline_rect(int x, int y, int w, int h, int thickness, int r, int g_, int b) {
    if (!g.backDC || w <= 0 || h <= 0 || thickness <= 0) return;
    if (thickness > w / 2 + 1) thickness = w / 2 + 1;
    if (thickness > h / 2 + 1) thickness = h / 2 + 1;
    HBRUSH br = fill_brush(r, g_, b);
    RECT top{ x, y, x + w, y + thickness };
    RECT bot{ x, y + h - thickness, x + w, y + h };
    RECT lft{ x, y + thickness, x + thickness, y + h - thickness };
    RECT rgt{ x + w - thickness, y + thickness, x + w, y + h - thickness };
    FillRect(g.backDC, &top, br);
    FillRect(g.backDC, &bot, br);
    FillRect(g.backDC, &lft, br);
    FillRect(g.backDC, &rgt, br);
}

extern "C" void engine_line(int x0, int y0, int x1, int y1, int r, int g_, int b) {
    if (!g.backDC) return;
    SetDCPenColor(g.backDC, RGB(r, g_, b));
    HPEN pen = (HPEN)GetStockObject(DC_PEN);
    HGDIOBJ old = SelectObject(g.backDC, pen);
    MoveToEx(g.backDC, x0, y0, nullptr);
    LineTo(g.backDC, x1, y1);
    LineTo(g.backDC, x1 + 1, y1);
    SelectObject(g.backDC, old);
}

extern "C" void engine_pixel(int x, int y, int r, int g_, int b) {
    if (!g.backDC) return;
    SetPixel(g.backDC, x, y, RGB(r, g_, b));
}

extern "C" void engine_text(int x, int y, const char* s, int r, int g_, int b) {
    if (!g.backDC || !s) return;
    SetBkMode(g.backDC, TRANSPARENT);
    SetTextAlign(g.backDC, TA_LEFT | TA_TOP);
    SetTextColor(g.backDC, RGB(r, g_, b));
    TextOutA(g.backDC, x, y, s, (int)strlen(s));
}

/* Column-major 5x7, bit 0 at the top. Copied from std/gfx so a 3D export
   and the editor draw the same letters. Advance is 6 px, a line is 8. */
static const unsigned char kFont5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x56,0x20,0x50}, {0x00,0x08,0x07,0x03,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x2A,0x1C,0x7F,0x1C,0x2A}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x00,0x60,0x60,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x72,0x49,0x49,0x49,0x46}, {0x21,0x41,0x49,0x4D,0x33}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x31}, {0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36}, {0x46,0x49,0x49,0x29,0x1E}, {0x00,0x00,0x14,0x00,0x00},
    {0x00,0x40,0x34,0x00,0x00}, {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x59,0x09,0x06}, {0x3E,0x41,0x5D,0x59,0x4E},
    {0x7C,0x12,0x11,0x12,0x7C}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x41,0x51,0x73}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x1C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x26,0x49,0x49,0x49,0x32}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x41,0x7F}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x03,0x07,0x08,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x18,0xA4,0xA4,0xA4,0x7C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x40,0x80,0x84,0x7D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x78,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0xFC,0x18,0x24,0x24,0x18},
    {0x18,0x24,0x24,0x18,0xFC}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x24},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x1C,0xA0,0xA0,0xA0,0x7C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x77,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08}
};

extern "C" void engine_text_px(int x, int y, const char* s, int r, int g_, int b, int scale) {
    if (!g.backDC || !s) return;
    if (scale < 1) scale = 1;
    if (scale > 64) scale = 64;
    HBRUSH brush = fill_brush(r, g_, b);
    int cx = x, cy = y;
    const int adv = 6 * scale;
    const int lh = 8 * scale;
    const int tab = 24 * scale;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        unsigned char ch = *p;
        if (ch == '\n') { cx = x; cy += lh; continue; }
        if (ch == '\t') {
            int cell = cx - x;
            if (cell < 0) cell = 0;
            cx = x + ((cell / tab) + 1) * tab;
            continue;
        }
        int glyph = (int)ch;
        if (glyph < 32 || glyph > 126) glyph = '?';
        const unsigned char* col = kFont5x7[glyph - 32];
        for (int i = 0; i < 5; ++i) {
            unsigned char bits = col[i];
            for (int j = 0; j < 8; ++j) {
                if (!(bits & (1u << j))) continue;
                RECT rc;
                rc.left = cx + i * scale;
                rc.top = cy + j * scale;
                rc.right = rc.left + scale;
                rc.bottom = rc.top + scale;
                FillRect(g.backDC, &rc, brush);
            }
        }
        cx += adv;
    }
}

extern "C" int engine_text_width(const char* s) {
    if (!g.backDC || !s) return 0;
    SIZE sz{};
    GetTextExtentPoint32A(g.backDC, s, (int)strlen(s), &sz);
    return sz.cx;
}

extern "C" int engine_text_height() {
    if (!g.backDC) return g.fontSize;
    TEXTMETRICA tm{};
    GetTextMetricsA(g.backDC, &tm);
    return tm.tmHeight;
}

extern "C" int engine_text_baseline_y(int rect_y, int rect_h) {
    // GDI TextOut and GDI+ DrawString both treat y as the top of the cell.
    // Center the ink (ascent), not the full cell, so unused descent does not lift the letters.
    int ink = g.fontSize;
    if (g.backDC) {
        TEXTMETRICA tm{};
        GetTextMetricsA(g.backDC, &tm);
        ink = tm.tmAscent;
        if (ink < 8) ink = tm.tmHeight;
    }
    return rect_y + (rect_h - ink) / 2;
}

extern "C" void engine_set_font_size(int px) {
    if (px < 6) px = 6;
    if (px > 160) px = 160;
    g.fontSize = px;
    rebuild_font();
}

extern "C" void engine_set_font(const char* face, int px) {
    if (face && *face) g.fontFace = face;
    engine_set_font_size(px);
}

extern "C" void engine_set_font_bold(int on) {
    g.fontBold = on ? 1 : 0;
    rebuild_font();
}

extern "C" void engine_quit() {
    g.quit = true;
}

static void gdip_add_round_rect(Gdiplus::GraphicsPath& path, int x, int y, int w, int h, int rad) {
    if (w < 1 || h < 1) return;
    if (rad < 0) rad = 0;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    Gdiplus::Rect r(x, y, w, h);
    if (rad <= 0) {
        path.AddRectangle(r);
        return;
    }
    int d = rad * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

extern "C" void engine_fill_rect_a(int x, int y, int w, int h, int r, int g_, int b, int a) {
    if (!g.backDC || w <= 0 || h <= 0) return;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    Gdiplus::Graphics gfx(g.backDC);
    Gdiplus::SolidBrush brush(Gdiplus::Color((BYTE)a, (BYTE)r, (BYTE)g_, (BYTE)b));
    gfx.FillRectangle(&brush, x, y, w, h);
}

extern "C" void engine_fill_round_a(int x, int y, int w, int h, int radius, int r, int g_, int b, int a) {
    if (!g.backDC || w <= 0 || h <= 0) return;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    Gdiplus::Graphics* gfx = round_gfx();
    if (!gfx) return;
    Gdiplus::GraphicsPath path;
    gdip_add_round_rect(path, x, y, w, h, radius);
    Gdiplus::SolidBrush brush(Gdiplus::Color((BYTE)a, (BYTE)r, (BYTE)g_, (BYTE)b));
    gfx->FillPath(&brush, &path);
    gfx->Flush(Gdiplus::FlushIntentionSync);
}

extern "C" void engine_outline_round_a(int x, int y, int w, int h, int radius, int thickness, int r, int g_, int b, int a) {
    if (!g.backDC || w <= 0 || h <= 0 || thickness <= 0) return;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    Gdiplus::Graphics* gfx = round_gfx();
    if (!gfx) return;
    Gdiplus::GraphicsPath path;
    gdip_add_round_rect(path, x, y, w, h, radius);
    Gdiplus::Pen pen(Gdiplus::Color((BYTE)a, (BYTE)r, (BYTE)g_, (BYTE)b), (Gdiplus::REAL)thickness);
    gfx->DrawPath(&pen, &path);
    gfx->Flush(Gdiplus::FlushIntentionSync);
}

extern "C" void engine_text_aa(int x, int y, const char* s, int r, int g_, int b, int a) {
    (void)a;
    // GDI+ DrawString + Comic Sans collapses glyph advances (SIZE→STZE, SEED→SFEED).
    // The DC font is already CLEARTYPE_QUALITY, so TextOut is antialiased and spaced correctly.
    engine_text(x, y, s, r, g_, b);
}

extern "C" void engine_set_clip(int x, int y, int w, int h) {
    if (!g.backDC) return;
    round_gfx_drop();
    HRGN rgn = CreateRectRgn(x, y, x + w, y + h);
    SelectClipRgn(g.backDC, rgn);
    DeleteObject(rgn);
}

extern "C" void engine_clear_clip() {
    if (!g.backDC) return;
    round_gfx_drop();
    SelectClipRgn(g.backDC, nullptr);
}

/* ============================================================ */
/*  Images                                                      */
/* ============================================================ */

/* forward decl: the body lives in the project section below, but image loaders
 * (which sit higher in the file) need to call it to resolve "assets/foo.png"
 * style paths against the currently-open project root. */
static std::string asset_resolve(const std::string& s);
static std::string proj_relativise(const std::string& s);

/* forward declared above; this is the body */
extern "C" int engine_image_load(const char* path) {
    if (!path) return 0;
    std::string resolved = asset_resolve(path);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, resolved.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return 0;
    std::wstring wpath((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, resolved.c_str(), -1, wpath.data(), wlen);

    auto* bmp = new Gdiplus::Bitmap(wpath.c_str());
    if (bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return 0;
    }

    int id = g.nextImageId++;
    Image img;
    img.bmp = bmp;
    img.w = (int)bmp->GetWidth();
    img.h = (int)bmp->GetHeight();
    image_ensure_dib(img);
    g.images[id] = img;
    return id;
}

extern "C" int engine_image_w(int id) {
    auto it = g.images.find(id);
    return it == g.images.end() ? 0 : it->second.w;
}

extern "C" int engine_image_h(int id) {
    auto it = g.images.find(id);
    return it == g.images.end() ? 0 : it->second.h;
}

extern "C" void engine_image_blit(int id, int x, int y) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp) return;
    Gdiplus::Graphics gfx(g.backDC);
    gfx.DrawImage(it->second.bmp, x, y, it->second.w, it->second.h);
}

extern "C" void engine_image_blit_scaled(int id, int x, int y, int w, int h) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp) return;
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    gfx.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    gfx.DrawImage(it->second.bmp, x, y, w, h);
}

/* White glyphs recolored to a theme color. Alpha stays, so the anti-aliased
   edge of an icon survives the tint. */
extern "C" void engine_image_blit_scaled_tint(int id, int x, int y, int w, int h, int r, int g_, int b) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp) return;
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (g_ < 0) g_ = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g_ > 255) g_ = 255;
    if (b > 255) b = 255;
    Gdiplus::ColorMatrix cm{};
    cm.m[0][0] = (Gdiplus::REAL)r / 255.0f;
    cm.m[1][1] = (Gdiplus::REAL)g_ / 255.0f;
    cm.m[2][2] = (Gdiplus::REAL)b / 255.0f;
    cm.m[3][3] = 1.0f;
    cm.m[4][4] = 1.0f;
    Gdiplus::ImageAttributes attr;
    attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    gfx.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    gfx.DrawImage(it->second.bmp,
                  Gdiplus::Rect(x, y, w, h),
                  0, 0, it->second.w, it->second.h,
                  Gdiplus::UnitPixel, &attr, nullptr, nullptr);
}

extern "C" void engine_image_blit_scaled_nn(int id, int x, int y, int w, int h) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp) return;
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    gfx.DrawImage(it->second.bmp, x, y, w, h);
}

extern "C" void engine_image_blit_sub_nn(int id, int src_x, int src_y, int src_w, int src_h,
                                         int dst_x, int dst_y, int dst_w, int dst_h) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp) return;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    gfx.DrawImage(it->second.bmp,
                  Gdiplus::Rect(dst_x, dst_y, dst_w, dst_h),
                  src_x, src_y, src_w, src_h,
                  Gdiplus::UnitPixel);
}

extern "C" void engine_image_blit_sub_nn_tint(int id, int src_x, int src_y, int src_w, int src_h,
                                              int dst_x, int dst_y, int dst_w, int dst_h,
                                              int r, int g_, int b) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp) return;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    if (r < 0) r = 0;
    if (g_ < 0) g_ = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g_ > 255) g_ = 255;
    if (b > 255) b = 255;
    Gdiplus::ColorMatrix cm{};
    cm.m[0][0] = (Gdiplus::REAL)r / 255.0f;
    cm.m[1][1] = (Gdiplus::REAL)g_ / 255.0f;
    cm.m[2][2] = (Gdiplus::REAL)b / 255.0f;
    cm.m[3][3] = 1.0f;
    cm.m[4][4] = 1.0f;
    Gdiplus::ImageAttributes attr;
    attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    gfx.DrawImage(it->second.bmp,
                  Gdiplus::Rect(dst_x, dst_y, dst_w, dst_h),
                  src_x, src_y, src_w, src_h,
                  Gdiplus::UnitPixel, &attr, nullptr, nullptr);
}

extern "C" void engine_image_blit_pan_nn(int id, double scroll_x, int view_w, int view_h) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC) return;
    if (view_w <= 0 || view_h <= 0) return;

    Image& img = it->second;
    const int world_w = img.w;
    const int world_h = img.h;
    if (world_w <= 0 || world_h <= 0) return;
    if (view_h > world_h) view_h = world_h;

    int ox = (int)scroll_x;
    ox %= world_w;
    if (ox < 0) ox += world_w;

    int first_w = world_w - ox;
    if (first_w > view_w) first_w = view_w;

    image_ensure_dib(img);
    if (img.hdc) {
        BitBlt(g.backDC, 0, 0, first_w, view_h, img.hdc, ox, 0, SRCCOPY);
        const int second_w = view_w - first_w;
        if (second_w > 0) {
            BitBlt(g.backDC, first_w, 0, second_w, view_h, img.hdc, 0, 0, SRCCOPY);
        }
        return;
    }

    if (!img.bmp) return;
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    gfx.DrawImage(img.bmp,
                  Gdiplus::Rect(0, 0, first_w, view_h),
                  ox, 0, first_w, view_h,
                  Gdiplus::UnitPixel);
    const int second_w = view_w - first_w;
    if (second_w > 0) {
        gfx.DrawImage(img.bmp,
                      Gdiplus::Rect(first_w, 0, second_w, view_h),
                      0, 0, second_w, view_h,
                      Gdiplus::UnitPixel);
    }
}

extern "C" void engine_fill_rect_rot_a(int x, int y, int w, int h, int r, int g_, int b, int a, int deg) {
    if (!g.backDC || w <= 0 || h <= 0) return;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.TranslateTransform((Gdiplus::REAL)(x + w * 0.5f), (Gdiplus::REAL)(y + h * 0.5f));
    gfx.RotateTransform((Gdiplus::REAL)deg);
    Gdiplus::SolidBrush brush(Gdiplus::Color((BYTE)a, (BYTE)r, (BYTE)g_, (BYTE)b));
    gfx.FillRectangle(&brush, (Gdiplus::REAL)(-w * 0.5f), (Gdiplus::REAL)(-h * 0.5f),
                      (Gdiplus::REAL)w, (Gdiplus::REAL)h);
}

extern "C" void engine_fill_rect_rot(int x, int y, int w, int h, int r, int g_, int b, int deg) {
    engine_fill_rect_rot_a(x, y, w, h, r, g_, b, 255, deg);
}

static void hsv_to_rgb(int h, int s, int v, int& R, int& G, int& B) {
    h %= 360;
    if (h < 0) h += 360;
    s = std::clamp(s, 0, 255);
    v = std::clamp(v, 0, 255);
    int region = h / 60;
    int rem = (h - region * 60) * 255 / 60;
    int p = v * (255 - s) / 255;
    int q = v * (255 - (s * rem) / 255) / 255;
    int t = v * (255 - (s * (255 - rem)) / 255) / 255;
    switch (region) {
    case 0: R = v; G = t; B = p; break;
    case 1: R = q; G = v; B = p; break;
    case 2: R = p; G = v; B = t; break;
    case 3: R = p; G = q; B = v; break;
    case 4: R = t; G = p; B = v; break;
    default: R = v; G = p; B = q; break;
    }
}

static void rgb_to_hsv(int r, int g_, int b, int& h, int& s, int& v) {
    r = std::clamp(r, 0, 255);
    g_ = std::clamp(g_, 0, 255);
    b = std::clamp(b, 0, 255);
    int maxv = std::max(r, std::max(g_, b));
    int minv = std::min(r, std::min(g_, b));
    v = maxv;
    int d = maxv - minv;
    s = maxv == 0 ? 0 : d * 255 / maxv;
    if (d == 0) { h = 0; return; }
    int hh = 0;
    if (maxv == r) hh = 60 * (g_ - b) / d;
    else if (maxv == g_) hh = 120 + 60 * (b - r) / d;
    else hh = 240 + 60 * (r - g_) / d;
    if (hh < 0) hh += 360;
    if (hh > 359) hh = 359;
    h = hh;
}

extern "C" int engine_rgb_hue(int r, int g_, int b) { int h, s, v; rgb_to_hsv(r, g_, b, h, s, v); return h; }
extern "C" int engine_rgb_sat(int r, int g_, int b) { int h, s, v; rgb_to_hsv(r, g_, b, h, s, v); return s; }
extern "C" int engine_rgb_val(int r, int g_, int b) { int h, s, v; rgb_to_hsv(r, g_, b, h, s, v); return v; }
extern "C" int engine_hsv_r(int h, int s, int v) { int R, G, B; hsv_to_rgb(h, s, v, R, G, B); return R; }
extern "C" int engine_hsv_g(int h, int s, int v) { int R, G, B; hsv_to_rgb(h, s, v, R, G, B); return G; }
extern "C" int engine_hsv_b(int h, int s, int v) { int R, G, B; hsv_to_rgb(h, s, v, R, G, B); return B; }

static void blit_bgra(int x, int y, int w, int h, const uint32_t* px) {
    if (!g.backDC || !px || w <= 0 || h <= 0) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(g.backDC, x, y, w, h, 0, 0, w, h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

static uint32_t bgra_of(int r, int g_, int b) {
    return (uint32_t)(std::clamp(b, 0, 255) | (std::clamp(g_, 0, 255) << 8) | (std::clamp(r, 0, 255) << 16));
}

/* Saturation across, value down. Painted here so the inspector does not
   issue one fill per pixel. */
extern "C" void engine_picker_sv(int x, int y, int w, int h, int hue) {
    if (w < 2 || h < 2 || w > 512 || h > 512) return;
    std::vector<uint32_t> px((size_t)w * h);
    for (int iy = 0; iy < h; ++iy) {
        int v = 255 - iy * 255 / (h - 1);
        for (int ix = 0; ix < w; ++ix) {
            int s = ix * 255 / (w - 1);
            int R, G, B;
            hsv_to_rgb(hue, s, v, R, G, B);
            px[(size_t)iy * w + ix] = bgra_of(R, G, B);
        }
    }
    blit_bgra(x, y, w, h, px.data());
}

extern "C" void engine_picker_hue(int x, int y, int w, int h) {
    if (w < 1 || h < 2 || w > 64 || h > 512) return;
    std::vector<uint32_t> px((size_t)w * h);
    for (int iy = 0; iy < h; ++iy) {
        int hue = iy * 359 / (h - 1);
        int R, G, B;
        hsv_to_rgb(hue, 255, 255, R, G, B);
        uint32_t c = bgra_of(R, G, B);
        for (int ix = 0; ix < w; ++ix) px[(size_t)iy * w + ix] = c;
    }
    blit_bgra(x, y, w, h, px.data());
}

/* Checkerboard with the color faded in from the left (transparent) to the right. */
extern "C" void engine_picker_alpha(int x, int y, int w, int h, int r, int g_, int b) {
    if (w < 2 || h < 1 || w > 512 || h > 64) return;
    r = std::clamp(r, 0, 255);
    g_ = std::clamp(g_, 0, 255);
    b = std::clamp(b, 0, 255);
    std::vector<uint32_t> px((size_t)w * h);
    for (int iy = 0; iy < h; ++iy) {
        for (int ix = 0; ix < w; ++ix) {
            int check = (((ix / 4) ^ (iy / 4)) & 1) ? 168 : 110;
            int al = ix * 255 / (w - 1);
            int cr = check + (r - check) * al / 255;
            int cg = check + (g_ - check) * al / 255;
            int cb = check + (b - check) * al / 255;
            px[(size_t)iy * w + ix] = bgra_of(cr, cg, cb);
        }
    }
    blit_bgra(x, y, w, h, px.data());
}

extern "C" void engine_image_blit_scaled_rot(int id, int x, int y, int w, int h, int deg) {
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp || w <= 0 || h <= 0) return;
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.TranslateTransform((Gdiplus::REAL)(x + w * 0.5f), (Gdiplus::REAL)(y + h * 0.5f));
    gfx.RotateTransform((Gdiplus::REAL)deg);
    gfx.DrawImage(it->second.bmp, (Gdiplus::REAL)(-w * 0.5f), (Gdiplus::REAL)(-h * 0.5f),
                  (Gdiplus::REAL)w, (Gdiplus::REAL)h);
}

extern "C" void engine_image_blit_scaled_rot_a(int id, int x, int y, int w, int h, int deg, int a) {
    if (a >= 255) {
        engine_image_blit_scaled_rot(id, x, y, w, h, deg);
        return;
    }
    auto it = g.images.find(id);
    if (it == g.images.end() || !g.backDC || !it->second.bmp || w <= 0 || h <= 0) return;
    if (a < 0) a = 0;
    Gdiplus::ColorMatrix cm{};
    cm.m[0][0] = 1.0f;
    cm.m[1][1] = 1.0f;
    cm.m[2][2] = 1.0f;
    cm.m[3][3] = (Gdiplus::REAL)a / 255.0f;
    cm.m[4][4] = 1.0f;
    Gdiplus::ImageAttributes attr;
    attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.TranslateTransform((Gdiplus::REAL)(x + w * 0.5f), (Gdiplus::REAL)(y + h * 0.5f));
    gfx.RotateTransform((Gdiplus::REAL)deg);
    Gdiplus::RectF dest((Gdiplus::REAL)(-w * 0.5f), (Gdiplus::REAL)(-h * 0.5f),
                        (Gdiplus::REAL)w, (Gdiplus::REAL)h);
    gfx.DrawImage(it->second.bmp, dest, 0, 0,
                  (Gdiplus::REAL)it->second.w, (Gdiplus::REAL)it->second.h,
                  Gdiplus::UnitPixel, &attr, nullptr, nullptr);
}

extern "C" void engine_image_free(int id) {
    auto it = g.images.find(id);
    if (it == g.images.end()) return;
    /* drop any path-cache entry that pointed at this id */
    for (auto pi = g.imageByPath.begin(); pi != g.imageByPath.end(); ) {
        if (pi->second == id) pi = g.imageByPath.erase(pi);
        else ++pi;
    }
    image_release_dib(it->second);
    delete it->second.bmp;
    g.images.erase(it);
}

/* Cache by path: editor thumbnails redraw the same paths every frame, so
 * decoding the PNG each time would be ridiculous. */
extern "C" int engine_image_load_cached(const char* path) {
    if (!path || !*path) return 0;
    auto it = g.imageByPath.find(path);
    if (it != g.imageByPath.end()) {
        if (g.images.find(it->second) != g.images.end()) return it->second;
        g.imageByPath.erase(it);
    }
    int id = engine_image_load(path);
    if (id != 0) g.imageByPath[path] = id;
    return id;
}

extern "C" void engine_image_cache_clear() {
    for (auto& kv : g.imageByPath) {
        auto it = g.images.find(kv.second);
        if (it != g.images.end()) {
            image_release_dib(it->second);
            delete it->second.bmp;
            g.images.erase(it);
        }
    }
    g.imageByPath.clear();
}

/* ============================================================ */
/*  Input                                                       */
/* ============================================================ */

extern "C" int engine_mouse_x() { return g.mouseX; }
extern "C" int engine_mouse_y() { return g.mouseY; }

static int g_uiOx = 0, g_uiOy = 0;
static float g_uiSx = 1.f, g_uiSy = 1.f;

extern "C" void engine_ui_set_map(int ox, int oy, int sx_milli, int sy_milli) {
    float sx = sx_milli / 1000.f;
    float sy = sy_milli / 1000.f;
    if (sx < 0.0001f) sx = 1.f;
    if (sy < 0.0001f) sy = 1.f;
    g_uiOx = ox;
    g_uiOy = oy;
    g_uiSx = sx;
    g_uiSy = sy;
}
extern "C" int engine_ui_screen_x(int x) { return g_uiOx + (int)std::lround(x * g_uiSx); }
extern "C" int engine_ui_screen_y(int y) { return g_uiOy + (int)std::lround(y * g_uiSy); }
extern "C" int engine_ui_screen_w(int w) {
    int v = (int)std::lround(w * g_uiSx);
    return v < 1 ? 1 : v;
}
extern "C" int engine_ui_screen_h(int h) {
    int v = (int)std::lround(h * g_uiSy);
    return v < 1 ? 1 : v;
}
extern "C" int engine_ui_hover(int x, int y, int w, int h) {
    int sx = engine_ui_screen_x(x);
    int sy = engine_ui_screen_y(y);
    int sw = engine_ui_screen_w(w);
    int sh = engine_ui_screen_h(h);
    return g.mouseX >= sx && g.mouseY >= sy && g.mouseX < sx + sw && g.mouseY < sy + sh ? 1 : 0;
}
extern "C" int engine_mouse_down(int btn) { return (btn >= 0 && btn < 3 && g.mouseDown[btn]) ? 1 : 0; }
extern "C" int engine_mouse_pressed(int btn) { return (btn >= 0 && btn < 3 && g.mousePressed[btn]) ? 1 : 0; }
extern "C" int engine_mouse_released(int btn) { return (btn >= 0 && btn < 3 && g.mouseReleased[btn]) ? 1 : 0; }
extern "C" int engine_mouse_wheel() { return g.mouseWheelAccum; }
extern "C" int engine_key_down(int vk) { return (vk >= 0 && vk < 256 && g.keyDown[vk]) ? 1 : 0; }
extern "C" int engine_key_pressed(int vk) { return (vk >= 0 && vk < 256 && g.keyPressed[vk]) ? 1 : 0; }
extern "C" int engine_key_released(int vk) { return (vk >= 0 && vk < 256 && g.keyReleased[vk]) ? 1 : 0; }
extern "C" int engine_typed_pop() {
    if (g.typedHead == g.typedTail) return 0;
    int c = g.typed[g.typedHead];
    g.typedHead = (g.typedHead + 1) % kTypedCap;
    return c;
}

/* ============================================================ */
/*  Time                                                        */
/* ============================================================ */

extern "C" double engine_now_ms() {
    LARGE_INTEGER cur{};
    QueryPerformanceCounter(&cur);
    return (double)(cur.QuadPart - g.perfStart.QuadPart) * 1000.0 / (double)g.perfFreq.QuadPart;
}

extern "C" void engine_sleep_ms(int ms) {
    if (ms > 0) Sleep((DWORD)ms);
}

extern "C" double engine_dt_seconds() { return g.dtSeconds; }
extern "C" void   engine_set_dt_seconds(double dt) { g.dtSeconds = dt; }

/* ============================================================ */
/*  Entity store                                                */
/* ============================================================ */

extern "C" int engine_entity_max()    { return kEntityMax; }
extern "C" int engine_entity_count()  {
    int n = 0;
    for (auto& e : g.entities) if (e.active) ++n;
    return n;
}
extern "C" int engine_entity_active(int s) { return active_slot(s) ? 1 : 0; }
extern "C" int engine_entity_id(int s) { return active_slot(s) ? g.entities[s].id : 0; }

extern "C" int engine_entity_x(int s) { return active_slot(s) ? g.entities[s].x : 0; }
extern "C" int engine_entity_y(int s) { return active_slot(s) ? g.entities[s].y : 0; }
extern "C" int engine_entity_w(int s) { return active_slot(s) ? g.entities[s].w : 0; }
extern "C" int engine_entity_h(int s) { return active_slot(s) ? g.entities[s].h : 0; }
extern "C" int engine_entity_rot(int s) { return active_slot(s) ? g.entities[s].rotDeg : 0; }
extern "C" int engine_entity_rot_x(int s) { return active_slot(s) ? g.entities[s].rotX : 0; }
extern "C" int engine_entity_rot_y(int s) { return active_slot(s) ? g.entities[s].rotY : 0; }
extern "C" int engine_entity_is_camera(int s) {
    return active_slot(s) && g.entities[s].isCamera ? 1 : 0;
}
extern "C" int engine_entity_color_r(int s) { return active_slot(s) ? g.entities[s].r : 0; }
extern "C" int engine_entity_color_g(int s) { return active_slot(s) ? g.entities[s].g : 0; }
extern "C" int engine_entity_color_b(int s) { return active_slot(s) ? g.entities[s].b : 0; }
extern "C" int engine_entity_color_a(int s) { return active_slot(s) ? g.entities[s].a : 255; }
extern "C" const char* engine_entity_name(int s)   { return active_slot(s) ? g.entities[s].name.c_str()   : ""; }
extern "C" const char* engine_entity_kind(int s)   { return active_slot(s) ? g.entities[s].kind.c_str()   : ""; }
extern "C" const char* engine_entity_text(int s)   { return active_slot(s) ? g.entities[s].text.c_str()   : ""; }
extern "C" const char* engine_entity_sprite(int s) { return active_slot(s) ? g.entities[s].sprite.c_str() : ""; }
extern "C" const char* engine_entity_script(int s) { return active_slot(s) ? g.entities[s].script.c_str() : ""; }
extern "C" int engine_entity_sprite_id(int s) { return active_slot(s) ? g.entities[s].spriteId : 0; }

static void entity_propagate_delta(int slot, int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (!active_slot(s)) continue;
        if (g.entities[s].parentSlot != slot) continue;
        g.entities[s].x += dx;
        g.entities[s].y += dy;
        entity_propagate_delta(s, dx, dy);
    }
}

static int g_physWriteback = 0;
static void physics_apply_entity(Entity& e);

extern "C" void engine_entity_set_x(int s, int v) {
    if (!active_slot(s)) return;
    int dx = v - g.entities[s].x;
    if (dx == 0) return;
    g.entities[s].x = v;
    entity_propagate_delta(s, dx, 0);
    physics_apply_entity(g.entities[s]);
}
extern "C" void engine_entity_set_y(int s, int v) {
    if (!active_slot(s)) return;
    int dy = v - g.entities[s].y;
    if (dy == 0) return;
    g.entities[s].y = v;
    entity_propagate_delta(s, 0, dy);
    physics_apply_entity(g.entities[s]);
}
extern "C" void engine_entity_set_w(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 4) v = 4;
    g.entities[s].w = v;
}
extern "C" void engine_entity_set_h(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 4) v = 4;
    g.entities[s].h = v;
}
static int wrap360(int v) {
    v %= 360;
    if (v < 0) v += 360;
    return v;
}

extern "C" void engine_entity_set_rot(int s, int v) {
    if (!active_slot(s)) return;
    v = wrap360(v);
    if (g.entities[s].rotDeg == v) return;
    g.entities[s].rotDeg = v;
    physics_apply_entity(g.entities[s]);
}
extern "C" void engine_entity_set_rot_x(int s, int v) {
    if (!active_slot(s)) return;
    g.entities[s].rotX = wrap360(v);
}
extern "C" void engine_entity_set_rot_y(int s, int v) {
    if (!active_slot(s)) return;
    g.entities[s].rotY = wrap360(v);
    g.entities[s].lookFine = false;
}
extern "C" void engine_entity_set_look(int s, float yaw, float pitch) {
    if (!active_slot(s)) return;
    Entity& e = g.entities[s];
    if (pitch > 89.f) pitch = 89.f;
    if (pitch < -89.f) pitch = -89.f;
    e.lookYaw = yaw;
    e.lookPitch = pitch;
    e.lookFine = true;
    e.rotY = wrap360((int)std::lround(yaw));
    int p = (int)std::lround(pitch);
    if (p < 0) p += 360;
    e.rotX = wrap360(p);
}
extern "C" float engine_entity_look_yaw(int s) {
    if (!active_slot(s)) return 0.f;
    if (g.entities[s].lookFine) return g.entities[s].lookYaw;
    return (float)g.entities[s].rotY;
}
extern "C" float engine_entity_look_pitch(int s) {
    if (!active_slot(s)) return 0.f;
    if (g.entities[s].lookFine) return g.entities[s].lookPitch;
    int p = g.entities[s].rotX;
    if (p > 180) p -= 360;
    return (float)p;
}

/* Column-major. R = Ry(rotY) * Rx(rotX) * Rz(-rotDeg).
   rotX = rotY = 0 is the old Z-only spin. */
static void mat4_ident(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.f;
}
static void mat4_mul(float* o, const float* a, const float* b) {
    float t[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            t[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1]
                         + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        }
    }
    std::memcpy(o, t, sizeof(t));
}
static void orient_matrix(float* m, int rotX, int rotY, int rotDeg) {
    constexpr float k = 0.017453292519943295f;
    float rz[16], rx[16], ry[16], t[16];
    mat4_ident(rz);
    mat4_ident(rx);
    mat4_ident(ry);
    float az = -(float)rotDeg * k, ax = (float)rotX * k, ay = (float)rotY * k;
    float cz = std::cos(az), sz = std::sin(az);
    float cx = std::cos(ax), sx = std::sin(ax);
    float cy = std::cos(ay), sy = std::sin(ay);
    rz[0] = cz; rz[4] = -sz; rz[1] = sz; rz[5] = cz;
    rx[5] = cx; rx[9] = -sx; rx[6] = sx; rx[10] = cx;
    ry[0] = cy; ry[8] = sy; ry[2] = -sy; ry[10] = cy;
    mat4_mul(t, rx, rz);
    mat4_mul(m, ry, t);
}
static void orient_extract(const float* m, int* rotX, int* rotY, int* rotDeg) {
    float sx = -m[9];
    if (sx > 1.f) sx = 1.f;
    if (sx < -1.f) sx = -1.f;
    float cx = std::sqrt(m[1] * m[1] + m[5] * m[5]);
    float pitch, yaw, roll;
    if (cx > 1e-4f) {
        pitch = std::atan2(sx, cx);
        yaw = std::atan2(m[8], m[10]);
        roll = std::atan2(m[1], m[5]);
    } else {
        pitch = std::atan2(sx, 0.f);
        yaw = std::atan2(-m[2], m[0]);
        roll = 0.f;
    }
    constexpr float k = 57.29577951308232f;
    *rotX = wrap360((int)std::lround(pitch * k));
    *rotY = wrap360((int)std::lround(yaw * k));
    *rotDeg = wrap360((int)std::lround(-roll * k));
}
static void axis_matrix(float* m, int axis, int deltaDeg) {
    mat4_ident(m);
    float a = (float)deltaDeg * 0.017453292519943295f;
    if (axis == 3) a = -a;
    float c = std::cos(a), s = std::sin(a);
    if (axis == 1) { m[5] = c; m[9] = -s; m[6] = s; m[10] = c; }
    else if (axis == 2) { m[0] = c; m[8] = s; m[2] = -s; m[10] = c; }
    else { m[0] = c; m[4] = -s; m[1] = s; m[5] = c; }
}

struct XGrab {
    int slot = -1;
    int x = 0, y = 0, z = 0, w = 0, h = 0, vis = 0;
    int rot = 0, rotX = 0, rotY = 0;
    int kind = 0;       /* 0 box, 1 cylinder, 2 sphere or model */
    int autoDepth = 0;
};
static std::vector<XGrab> g_xgrab;

static int entity_visual_depth(const Entity& e) {
    if (e.depth > 0) return e.depth;
    if (e.kind == "cube" || e.kind == "cylinder") return std::max(4, e.w);
    if (e.kind == "model") return std::max(4, std::max(e.w, e.h));
    return std::max(4, std::min(e.w, e.h));
}
static int scale_dim(int base, double factor) {
    int v = (int)std::lround((double)base * factor);
    if (v < 4) v = 4;
    if (v > 100000) v = 100000;
    return v;
}

extern "C" void engine_xform_grab_begin(void) { g_xgrab.clear(); }
extern "C" void engine_xform_grab_add(int slot) {
    if (!active_slot(slot)) return;
    for (const XGrab& it : g_xgrab) if (it.slot == slot) return;
    const Entity& e = g.entities[slot];
    XGrab row;
    row.slot = slot;
    row.x = e.x; row.y = e.y; row.z = e.z;
    row.w = e.w; row.h = e.h;
    row.vis = entity_visual_depth(e);
    row.rot = e.rotDeg; row.rotX = e.rotX; row.rotY = e.rotY;
    row.autoDepth = e.depth <= 0 ? 1 : 0;
    if (e.kind == "sphere" || e.kind == "model") row.kind = 2;
    else if (e.kind == "cylinder" || e.kind == "capsule") row.kind = 1;
    g_xgrab.push_back(row);
}
extern "C" void engine_xform_grab_scale(double factor, int axis) {
    if (factor < 0.05) factor = 0.05;
    if (factor > 6.0) factor = 6.0;
    if (axis < 0 || axis > 3) axis = 0;
    for (const XGrab& row : g_xgrab) {
        if (!active_slot(row.slot)) continue;
        int ax = axis;
        if (row.kind == 2) ax = 0;
        bool dw = false, dh = false, dd = false;
        if (ax == 0) { dw = dh = dd = true; }
        else if (row.kind == 1) {
            if (ax == 2) dh = true;
            else { dw = true; dd = true; }
        } else {
            if (ax == 1) dw = true;
            if (ax == 2) dh = true;
            if (ax == 3) dd = true;
        }
        int nw = row.w, nh = row.h, nv = row.vis;
        if (row.kind == 2) {
            int s = scale_dim(std::max(row.w, row.h), factor);
            nw = nh = nv = s;
            dw = dh = dd = true;
        } else {
            if (dw) nw = scale_dim(row.w, factor);
            if (dh) nh = scale_dim(row.h, factor);
            if (dd) nv = scale_dim(row.vis, factor);
        }
        if (nw != row.w) engine_entity_set_w(row.slot, nw);
        if (nh != row.h) engine_entity_set_h(row.slot, nh);
        if (dw) engine_entity_set_x(row.slot, (int)std::lround((row.x + row.w / 2.0) - nw / 2.0));
        if (dh) engine_entity_set_y(row.slot, (int)std::lround((row.y + row.h / 2.0) - nh / 2.0));
        if (dd) {
            engine_entity_set_depth(row.slot, nv);
            engine_entity_set_z(row.slot, (int)std::lround((row.z + row.vis / 2.0) - nv / 2.0));
        } else if (row.autoDepth && (dw || dh)) {
            /* depth 0 follows width. Pin it so this axis does not drag the others. */
            engine_entity_set_depth(row.slot, row.vis);
        }
    }
}
extern "C" void engine_xform_grab_spin(int axis, int deltaDeg) {
    if (axis < 1 || axis > 3) return;
    for (const XGrab& row : g_xgrab) {
        if (!active_slot(row.slot)) continue;
        float base[16], turn[16], out[16];
        orient_matrix(base, row.rotX, row.rotY, row.rot);
        axis_matrix(turn, axis, deltaDeg);
        mat4_mul(out, turn, base);
        int rx = 0, ry = 0, rz = 0;
        orient_extract(out, &rx, &ry, &rz);
        engine_entity_set_rot_x(row.slot, rx);
        engine_entity_set_rot_y(row.slot, ry);
        engine_entity_set_rot(row.slot, rz);
    }
}
extern "C" int engine_entity_z(int s)     { return active_slot(s) ? g.entities[s].z : 0; }
extern "C" int engine_entity_depth(int s) { return active_slot(s) ? g.entities[s].depth : 0; }
extern "C" const char* engine_entity_model(int s) { return active_slot(s) ? g.entities[s].model.c_str() : ""; }

static void entity_propagate_dz(int slot, int dz) {
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (!active_slot(s) || g.entities[s].parentSlot != slot) continue;
        g.entities[s].z += dz;
        entity_propagate_dz(s, dz);
    }
}
extern "C" void engine_entity_set_z(int s, int v) {
    if (!active_slot(s)) return;
    int dz = v - g.entities[s].z;
    if (dz == 0) return;
    g.entities[s].z = v;
    entity_propagate_dz(s, dz);
}
extern "C" void engine_entity_set_depth(int s, int v) {
    if (!active_slot(s)) return;
    g.entities[s].depth = v < 0 ? 0 : v;
}
extern "C" int engine_entity_light_intensity(int s) { return active_slot(s) ? g.entities[s].lightIntensity : 0; }
extern "C" int engine_entity_light_range(int s)     { return active_slot(s) ? g.entities[s].lightRange : 0; }
extern "C" int engine_entity_light_angle(int s)     { return active_slot(s) ? g.entities[s].lightAngle : 0; }
extern "C" int engine_entity_light_tilt(int s)      { return active_slot(s) ? g.entities[s].lightTilt : 0; }
extern "C" void engine_entity_set_light_intensity(int s, int v) {
    if (active_slot(s)) g.entities[s].lightIntensity = std::clamp(v, 0, 1000);
}
extern "C" void engine_entity_set_light_range(int s, int v) {
    if (active_slot(s)) g.entities[s].lightRange = std::clamp(v, 1, 100000);
}
extern "C" void engine_entity_set_light_angle(int s, int v) {
    if (active_slot(s)) g.entities[s].lightAngle = std::clamp(v, 1, 179);
}
extern "C" void engine_entity_set_light_tilt(int s, int v) {
    if (active_slot(s)) g.entities[s].lightTilt = std::clamp(v, -90, 90);
}
extern "C" int engine_entity_visible(int s) {
    return active_slot(s) && g.entities[s].visible ? 1 : 0;
}
extern "C" void engine_entity_set_visible(int s, int on) {
    if (active_slot(s)) g.entities[s].visible = on != 0;
}

static int clamp_ch(int v) { return std::clamp(v, 0, 255); }

static bool entity_is_button(const Entity& e) {
    if (e.kind == "ui_button") return true;
    if (e.kind == "rect" && e.name == "button") return true;
    return false;
}

static Entity::BtnTint* button_tint(Entity& e, int which) {
    if (which == 1) return &e.btnPress;
    if (which == 2) return &e.btnOn;
    if (which == 3) return &e.btnOnHover;
    return &e.btnHover;
}
static const Entity::BtnTint* button_tint(const Entity& e, int which) {
    if (which == 1) return &e.btnPress;
    if (which == 2) return &e.btnOn;
    if (which == 3) return &e.btnOnHover;
    return &e.btnHover;
}
static int tint_ch(const Entity::BtnTint& t, int channel) {
    if (channel == 1) return t.g;
    if (channel == 2) return t.b;
    return t.r;
}

/* which 0 hover, 1 pressed, 2 on, 3 on+hover. Ignores the pointer. */
static void button_resolved_rgb(const Entity& e, int which, int& r, int& g, int& b) {
    const Entity::BtnTint* t = button_tint(e, which);
    if (which == 2) {
        if (t->custom) { r = t->r; g = t->g; b = t->b; }
        else { r = clamp_ch(e.r + 36); g = clamp_ch(e.g + 36); b = clamp_ch(e.b + 36); }
        return;
    }
    if (which == 3) {
        int or_, og, ob;
        button_resolved_rgb(e, 2, or_, og, ob);
        if (t->custom) { r = t->r; g = t->g; b = t->b; }
        else { r = clamp_ch(or_ - 36); g = clamp_ch(og - 36); b = clamp_ch(ob - 36); }
        return;
    }
    if (t->custom) { r = t->r; g = t->g; b = t->b; return; }
    int amt = (which == 1) ? 56 : 36;
    r = clamp_ch(e.r - amt);
    g = clamp_ch(e.g - amt);
    b = clamp_ch(e.b - amt);
}

extern "C" int engine_entity_button_color(int slot, int which, int channel) {
    if (!active_slot(slot)) return 0;
    return tint_ch(*button_tint(g.entities[slot], which), channel);
}
extern "C" void engine_entity_set_button_color(int slot, int which, int r, int g_, int b) {
    if (!active_slot(slot)) return;
    Entity::BtnTint* t = button_tint(g.entities[slot], which);
    t->r = clamp_ch(r); t->g = clamp_ch(g_); t->b = clamp_ch(b); t->custom = 1;
}
extern "C" int engine_entity_button_custom(int slot, int which) {
    if (!active_slot(slot)) return 0;
    return button_tint(g.entities[slot], which)->custom ? 1 : 0;
}
extern "C" void engine_entity_set_button_custom(int slot, int which, int on) {
    if (!active_slot(slot)) return;
    button_tint(g.entities[slot], which)->custom = on != 0;
}
extern "C" int engine_entity_button_resolved(int slot, int which, int channel) {
    if (!active_slot(slot)) return 0;
    int r = 0, gg = 0, b = 0;
    button_resolved_rgb(g.entities[slot], which, r, gg, b);
    if (channel == 1) return gg;
    if (channel == 2) return b;
    return r;
}
extern "C" int engine_entity_button_fill(int slot, int hovered, int held, int channel) {
    if (!active_slot(slot)) return 0;
    const Entity& e = g.entities[slot];
    int r = e.r, g = e.g, b = e.b;
    if (!entity_is_button(e)) {
        if (channel == 1) return g;
        if (channel == 2) return b;
        return r;
    }
    if (e.btnToggle && e.btnToggled) button_resolved_rgb(e, 2, r, g, b);
    if (held) {
        if (e.btnPress.custom) { r = e.btnPress.r; g = e.btnPress.g; b = e.btnPress.b; }
        else { r = clamp_ch(r - 28); g = clamp_ch(g - 28); b = clamp_ch(b - 28); }
    } else if (hovered) {
        if (e.btnToggle && e.btnToggled) button_resolved_rgb(e, 3, r, g, b);
        else button_resolved_rgb(e, 0, r, g, b);
    }
    if (channel == 1) return g;
    if (channel == 2) return b;
    return r;
}
extern "C" int engine_entity_button_toggle(int slot) {
    return active_slot(slot) && g.entities[slot].btnToggle ? 1 : 0;
}
extern "C" void engine_entity_set_button_toggle(int slot, int on) {
    if (active_slot(slot)) g.entities[slot].btnToggle = on != 0;
}
extern "C" int engine_entity_button_toggled(int slot) {
    return active_slot(slot) && g.entities[slot].btnToggled ? 1 : 0;
}
extern "C" void engine_entity_set_button_toggled(int slot, int on) {
    if (active_slot(slot)) g.entities[slot].btnToggled = on != 0;
}
extern "C" void engine_entity_copy_button(int dst, int src) {
    if (!active_slot(dst) || !active_slot(src)) return;
    Entity& d = g.entities[dst];
    const Entity& s = g.entities[src];
    d.btnHover = s.btnHover;
    d.btnPress = s.btnPress;
    d.btnOn = s.btnOn;
    d.btnOnHover = s.btnOnHover;
    d.btnToggle = s.btnToggle;
    d.btnToggled = s.btnToggled;
    d.btnScript = s.btnScript;
    d.btnFn = s.btnFn;
}

static std::string button_action_text(const char* s) {
    std::string v = s ? s : "";
    std::string o;
    o.reserve(v.size());
    for (unsigned char c : v) {
        if (c == '|' || c == '\n' || c == '\r') continue;
        o += (char)c;
        if (o.size() >= 240) break;
    }
    return o;
}

extern "C" const char* engine_entity_button_script(int slot) {
    return active_slot(slot) ? g.entities[slot].btnScript.c_str() : "";
}
extern "C" void engine_entity_set_button_script(int slot, const char* s) {
    if (!active_slot(slot)) return;
    g.entities[slot].btnScript = button_action_text(s);
}
extern "C" const char* engine_entity_button_fn(int slot) {
    return active_slot(slot) ? g.entities[slot].btnFn.c_str() : "";
}
extern "C" void engine_entity_set_button_fn(int slot, const char* s) {
    if (!active_slot(slot)) return;
    g.entities[slot].btnFn = button_action_text(s);
}
extern "C" int engine_entity_shown(int s) {
    return entity_shown(s) ? 1 : 0;
}

/* Scene-wide render mode: 0 = 2D (editor play / EyescirePlayer), 1 = 3D
 * (EyescirePlayer3D). Saved as a RENDER line. */
extern "C" int  engine_scene_render3d() { return g.render3d ? 1 : 0; }
extern "C" void engine_scene_set_render3d(int on) { g.render3d = on != 0; }
extern "C" int engine_scene_clear_r() { return g.clearR; }
extern "C" int engine_scene_clear_g() { return g.clearG; }
extern "C" int engine_scene_clear_b() { return g.clearB; }
extern "C" void engine_scene_set_clear(int r, int g_, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g_ < 0) g_ = 0; if (g_ > 255) g_ = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    g.clearR = r; g.clearG = g_; g.clearB = b;
}
extern "C" float engine_scene_fov() { return g.sceneFov; }
extern "C" void engine_scene_set_fov(float deg) {
    if (deg < 20.0f) deg = 20.0f;
    if (deg > 120.0f) deg = 120.0f;
    g.sceneFov = deg;
}

extern "C" int engine_scene_sky() {
    if (g.skyMode < 0 || g.skyMode > 4) return 0;
    return g.skyMode;
}
extern "C" void engine_scene_set_sky(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 4) mode = 4;
    g.skyMode = mode;
}
extern "C" const char* engine_scene_sky_face(int face) {
    if (face < 0 || face > 5) return "";
    return g.skyFace[face].c_str();
}
extern "C" void engine_scene_set_sky_face(int face, const char* path) {
    if (face < 0 || face > 5) return;
    std::string clean = path ? path : "";
    for (char& c : clean) if (c == '|' || c == '\r' || c == '\n') c = ' ';
    g.skyFace[face] = proj_relativise(clean);
}

extern "C" void engine_entity_set_camera(int s, int on) {
    if (!active_slot(s)) return;
    if (on) {
        for (Entity& e : g.entities)
            if (e.active) e.isCamera = false;
        g.entities[s].isCamera = true;
    } else {
        g.entities[s].isCamera = false;
    }
}
extern "C" int engine_find_main_camera() {
    for (int i = 0; i < (int)g.entities.size(); ++i)
        if (g.entities[i].active && g.entities[i].isCamera) return i;
    return -1;
}
extern "C" void engine_entity_set_color(int s, int r, int g_, int b) {
    if (!active_slot(s)) return;
    g.entities[s].r = clamp_ch(r);
    g.entities[s].g = clamp_ch(g_);
    g.entities[s].b = clamp_ch(b);
}
extern "C" void engine_entity_set_alpha(int s, int a) {
    if (!active_slot(s)) return;
    g.entities[s].a = clamp_ch(a);
}
extern "C" void engine_entity_set_name(int s, const char* v) {
    if (active_slot(s) && v) g.entities[s].name = v;
}
extern "C" void engine_entity_set_kind(int s, const char* v) {
    if (active_slot(s) && v) g.entities[s].kind = v;
}
extern "C" void engine_entity_set_text(int s, const char* v) {
    if (active_slot(s) && v) g.entities[s].text = v;
}
/* If `s` looks like a path inside the currently-open project, return it as a
 * forward-slashed relative path (e.g. "assets/player.png") so the scene file
 * stays portable. Otherwise return it unchanged. */
static std::string proj_relativise(const std::string& s) {
    if (s.empty()) return s;
    if (!g.projLoaded || g.projRoot.empty()) return s;
    std::error_code ec;
    fs::path p(s);
    if (!p.is_absolute()) return s;
    fs::path rel = fs::relative(p, fs::path(g.projRoot), ec);
    if (ec) return s;
    std::string r = rel.generic_string();
    /* a "../foo" relative path means it's outside the project root; keep absolute. */
    if (r.empty() || r.rfind("..", 0) == 0) return s;
    return r;
}

/* Inverse of proj_relativise: turn a possibly-relative asset path into an
 * absolute path we can hand to the image loader. */
static std::string asset_resolve(const std::string& s) {
    if (s.empty()) return s;
    fs::path p(s);
    if (p.is_absolute()) return s;
    if (g.projLoaded && !g.projRoot.empty()) {
        return (fs::path(g.projRoot) / p).string();
    }
    return s;
}

/* A .ttf beside the project is not an installed Windows font. Reading its
   family name and adding it for this process lets labels draw with it. */
static bool path_is_font_file(const std::string& p) {
    if (p.size() < 4) return false;
    std::string e = p.substr(p.size() - 4);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e == ".ttf" || e == ".otf";
}

static std::string ttf_family_name(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 12) return {};
    auto ru16 = [&](size_t o) -> int {
        if (o + 2 > b.size()) return 0;
        return ((int)b[o] << 8) | (int)b[o + 1];
    };
    auto ru32 = [&](size_t o) -> unsigned {
        if (o + 4 > b.size()) return 0;
        return ((unsigned)b[o] << 24) | ((unsigned)b[o + 1] << 16) |
               ((unsigned)b[o + 2] << 8) | (unsigned)b[o + 3];
    };
    int ntab = ru16(4);
    unsigned nameOff = 0, nameLen = 0;
    for (int i = 0; i < ntab; ++i) {
        size_t o = 12 + (size_t)i * 16;
        if (o + 16 > b.size()) break;
        if (b[o] == 'n' && b[o + 1] == 'a' && b[o + 2] == 'm' && b[o + 3] == 'e') {
            nameOff = ru32(o + 8);
            nameLen = ru32(o + 12);
        }
    }
    if (nameLen < 6 || nameOff + nameLen > b.size()) return {};
    int count = ru16(nameOff + 2);
    int store = ru16(nameOff + 4);
    std::string mac, win;
    for (int i = 0; i < count; ++i) {
        size_t o = nameOff + 6 + (size_t)i * 12;
        if (o + 12 > b.size()) break;
        int plat = ru16(o), nid = ru16(o + 6), len = ru16(o + 8), noff = ru16(o + 10);
        if (nid != 1) continue;
        size_t s = nameOff + store + noff;
        if (s + len > b.size()) continue;
        std::string face;
        if (plat == 3 || plat == 0) {
            for (int k = 0; k + 1 < len; k += 2)
                face.push_back((char)b[s + k + 1]);
        } else {
            face.assign((const char*)b.data() + s, (size_t)len);
        }
        if (face.empty()) continue;
        if (plat == 3) win = face;
        else if (mac.empty()) mac = face;
    }
    return !win.empty() ? win : mac;
}

static std::unordered_map<std::string, std::string> g_fontFamilies;
static std::unordered_map<std::string, HFONT> g_fileFonts;

static std::string font_family_for(const std::string& rel) {
    if (!path_is_font_file(rel)) return {};
    std::string full = asset_resolve(rel);
    auto it = g_fontFamilies.find(full);
    if (it != g_fontFamilies.end()) return it->second;
    std::string face = ttf_family_name(full);
    if (face.empty()) {
        g_fontFamilies[full] = "";
        return {};
    }
    int n = MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, full.c_str(), -1, w.data(), n);
    if (AddFontResourceExW(w.c_str(), 0x10 /* FR_PRIVATE */, nullptr) == 0) {
        g_fontFamilies[full] = "";
        return {};
    }
    g_fontFamilies[full] = face;
    return face;
}

static HFONT font_cached(const std::string& face, int px) {
    if (px < 6) px = 6;
    if (px > 400) px = 400;
    std::string key = face + "|" + std::to_string(px) + "|file";
    auto it = g_fileFonts.find(key);
    if (it != g_fileFonts.end()) return it->second;
    HFONT f = CreateFontA(
        -px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_DONTCARE, face.c_str());
    if (f) g_fileFonts[key] = f;
    return f;
}

static HFONT font_select_file(const char* path, int px) {
    if (!g.backDC || !path || !*path) return nullptr;
    std::string face = font_family_for(path);
    if (face.empty()) return nullptr;
    HFONT f = font_cached(face, px);
    if (!f) return nullptr;
    SelectObject(g.backDC, f);
    return f;
}

extern "C" int engine_text_file_width(const char* s, int px, const char* path) {
    if (!s) return 0;
    HFONT prev = g.font;
    if (!font_select_file(path, px)) {
        if (prev) SelectObject(g.backDC, prev);
        return 0;
    }
    int w = engine_text_width(s);
    if (prev) SelectObject(g.backDC, prev);
    return w;
}

extern "C" void engine_text_file(int x, int y, const char* s, int r, int g_, int b, int px, const char* path) {
    HFONT prev = g.font;
    if (!font_select_file(path, px)) {
        if (g.backDC && prev) SelectObject(g.backDC, prev);
        return;
    }
    engine_text(x, y, s, r, g_, b);
    if (prev) SelectObject(g.backDC, prev);
}

static bool music_path_is_wav(const std::string& p) {
    if (p.size() < 4) return false;
    std::string ext = p.substr(p.size() - 4);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return ext == ".wav";
}

static void music_apply_volume() {
    DWORD v = (DWORD)((g.musicVolume * 65535) / 1000);
    DWORD vol = v | (v << 16);
    waveOutSetVolume(nullptr, vol);

    if (!g.musicPlaying) return;
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "setaudio eyescireMusic volume to %d", g.musicVolume);
    mciSendStringA(cmd, nullptr, 0, nullptr);
}

static void music_close() {
    PlaySoundA(nullptr, nullptr, 0);
    mciSendStringA("stop eyescireMusic", nullptr, 0, nullptr);
    mciSendStringA("close eyescireMusic", nullptr, 0, nullptr);
    g.musicPlaying = false;
}

extern "C" int engine_music_play(const char* path, int loop) {
    if (!path || !*path) return 0;
    music_close();

    std::string resolved = asset_resolve(path);
    if (!fs::exists(resolved)) {
        std::string fallback = std::string(engine_exe_dir()) + "\\" + path;
        if (fs::exists(fallback)) resolved = fallback;
        else {
            log_push(std::string("[music] missing: ") + path);
            return 0;
        }
    }

    music_apply_volume();

    if (music_path_is_wav(resolved)) {
        UINT flags = SND_FILENAME | SND_ASYNC;
        if (loop) flags |= SND_LOOP;
        if (PlaySoundA(resolved.c_str(), nullptr, flags)) {
            g.musicPlaying = true;
            return 1;
        }
        log_push(std::string("[music] PlaySound failed: ") + resolved);
        return 0;
    }

    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "open \"%s\" type mpegvideo alias eyescireMusic", resolved.c_str());
    if (mciSendStringA(cmd, nullptr, 0, nullptr) != 0) {
        log_push(std::string("[music] MCI open failed: ") + resolved);
        return 0;
    }

    const char* playCmd = loop ? "play eyescireMusic repeat" : "play eyescireMusic";
    if (mciSendStringA(playCmd, nullptr, 0, nullptr) != 0) {
        mciSendStringA("close eyescireMusic", nullptr, 0, nullptr);
        log_push(std::string("[music] MCI play failed: ") + resolved);
        return 0;
    }

    g.musicPlaying = true;
    return 1;
}

extern "C" void engine_music_stop() {
    music_close();
}

extern "C" int engine_music_playing() {
    return g.musicPlaying ? 1 : 0;
}

extern "C" void engine_music_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g.musicVolume = percent * 10;
    music_apply_volume();
}

extern "C" int engine_music_volume() {
    return g.musicVolume / 10;
}

extern "C" void engine_sfx_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g.sfxVolume = percent * 10;
}

extern "C" int engine_sfx_volume() {
    return g.sfxVolume / 10;
}

extern "C" void engine_set_autosave(int on) {
    g.autosaveEnabled = (on != 0);
}

extern "C" int engine_autosave() {
    return g.autosaveEnabled ? 1 : 0;
}

extern "C" void engine_entity_set_sprite(int s, const char* v) {
    if (!active_slot(s) || !v) return;
    Entity& e = g.entities[s];
    if (e.spriteId != 0) {
        engine_image_free(e.spriteId);
        e.spriteId = 0;
    }
    e.sprite = proj_relativise(v);
    if (!e.sprite.empty()) {
        std::string p = asset_resolve(e.sprite);
        e.spriteId = engine_image_load(p.c_str());
    }
}
extern "C" void engine_entity_set_script(int s, const char* v) {
    if (active_slot(s) && v) g.entities[s].script = v;
}
extern "C" void engine_entity_set_model(int s, const char* v) {
    if (active_slot(s) && v) g.entities[s].model = proj_relativise(v);
}
extern "C" const char* engine_entity_sound(int s) {
    return active_slot(s) ? g.entities[s].sound.c_str() : "";
}
extern "C" void engine_entity_set_sound(int s, const char* v) {
    if (!active_slot(s) || !v) return;
    std::string clean;
    for (char c : std::string(v)) if (c != '|' && c != '\r' && c != '\n') clean.push_back(c);
    g.entities[s].sound = proj_relativise(clean);
}
extern "C" int engine_entity_sound_loop(int s) {
    return active_slot(s) && g.entities[s].soundLoop ? 1 : 0;
}
extern "C" void engine_entity_set_sound_loop(int s, int on) {
    if (active_slot(s)) g.entities[s].soundLoop = on ? 1 : 0;
}
extern "C" int engine_entity_sound_range(int s) {
    return active_slot(s) ? g.entities[s].soundRange : 800;
}
extern "C" void engine_entity_set_sound_range(int s, int px) {
    if (!active_slot(s)) return;
    if (px < 50) px = 50;
    if (px > 20000) px = 20000;
    g.entities[s].soundRange = px;
}
extern "C" int engine_entity_emit_rate(int s) { return active_slot(s) ? g.entities[s].emitRate : 20; }
extern "C" void engine_entity_set_emit_rate(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    g.entities[s].emitRate = v;
}
extern "C" int engine_entity_emit_life(int s) { return active_slot(s) ? g.entities[s].emitLife : 900; }
extern "C" void engine_entity_set_emit_life(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 50) v = 50;
    if (v > 10000) v = 10000;
    g.entities[s].emitLife = v;
}
extern "C" int engine_entity_emit_speed(int s) { return active_slot(s) ? g.entities[s].emitSpeed : 80; }
extern "C" void engine_entity_set_emit_speed(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 0) v = 0;
    if (v > 2000) v = 2000;
    g.entities[s].emitSpeed = v;
}
extern "C" int engine_entity_emit_spread(int s) { return active_slot(s) ? g.entities[s].emitSpread : 40; }
extern "C" void engine_entity_set_emit_spread(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 0) v = 0;
    if (v > 180) v = 180;
    g.entities[s].emitSpread = v;
}
extern "C" int engine_entity_emit_size(int s) { return active_slot(s) ? g.entities[s].emitSize : 28; }
extern "C" void engine_entity_set_emit_size(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 2) v = 2;
    if (v > 256) v = 256;
    g.entities[s].emitSize = v;
}
extern "C" int engine_entity_emit_hd(int s) { return active_slot(s) && g.entities[s].emitHD ? 1 : 0; }
extern "C" void engine_entity_set_emit_hd(int s, int on) {
    if (active_slot(s)) g.entities[s].emitHD = on ? 1 : 0;
}
/* Absolute path of an entity's model (relative ones resolve like sprites). */
extern "C" const char* engine_entity_model_path(int s) {
    static std::string cached;
    cached.clear();
    if (active_slot(s) && !g.entities[s].model.empty()) cached = asset_resolve(g.entities[s].model);
    return cached.c_str();
}

struct FpsBody { float vy = 0.f; int grounded = 1; };
static std::unordered_map<int, FpsBody> g_fps;

static void destroy_subtree(int s) {
    if (!active_slot(s)) return;
    for (int c = 0; c < (int)g.entities.size(); ++c) {
        if (active_slot(c) && g.entities[c].parentSlot == s)
            destroy_subtree(c);
    }
    Entity& e = g.entities[s];
    int gone = e.id;
    if (e.body && g.physWorld) {
        g.physWorld->DestroyBody(e.body);
        e.body = nullptr;
    }
    if (e.spriteId != 0) { engine_image_free(e.spriteId); e.spriteId = 0; }
    engine_bp_values_forget(gone);
    g_fps.erase(gone);
    e = Entity{};
}

extern "C" int engine_entity_create(const char* kind, const char* name,
                                    int x, int y, int w, int h,
                                    int r, int g_, int b) {
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (!g.entities[s].active) {
            Entity& e = g.entities[s];
            e = Entity{};
            e.active = true;
            e.id     = g.nextEntityId++;
            e.kind   = kind ? kind : "rect";
            e.name   = name ? name : "entity";
            e.x = x; e.y = y; e.w = (w < 4 ? 4 : w); e.h = (h < 4 ? 4 : h);
            e.r = r; e.g = g_; e.b = b;
            return s;
        }
    }
    return -1;
}

extern "C" void engine_entity_destroy(int s) {
    destroy_subtree(s);
}

extern "C" void engine_entity_clear_all() {
    engine_bp_values_clear();
    g_fps.clear();
    for (auto& e : g.entities) {
        if (e.active && e.spriteId != 0) engine_image_free(e.spriteId);
        e = Entity{};
    }
    g.nextEntityId = 1;
    g.selfSlot     = -1;
}

extern "C" int engine_entity_find_by_name(const char* name) {
    if (!name) return -1;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (g.entities[s].active && g.entities[s].name == name) return s;
    }
    return -1;
}

extern "C" int engine_entity_find_by_id(int id) {
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (g.entities[s].active && g.entities[s].id == id) return s;
    }
    return -1;
}

extern "C" int engine_entity_iter_next(int from) {
    if (from < 0) from = 0;
    for (int s = from; s < (int)g.entities.size(); ++s) {
        if (g.entities[s].active) return s;
    }
    return -1;
}

extern "C" int engine_entity_parent(int slot) {
    if (!active_slot(slot)) return -1;
    return g.entities[slot].parentSlot;
}

static bool entity_would_cycle(int slot, int new_parent) {
    if (new_parent < 0) return false;
    if (slot == new_parent) return true;
    int cur = new_parent;
    while (cur >= 0 && active_slot(cur)) {
        if (cur == slot) return true;
        cur = g.entities[cur].parentSlot;
    }
    return false;
}

extern "C" int engine_entity_set_parent(int slot, int parent_slot) {
    if (!active_slot(slot)) return 0;
    if (parent_slot >= 0 && !active_slot(parent_slot)) return 0;
    if (entity_would_cycle(slot, parent_slot)) return 0;
    g.entities[slot].parentSlot = parent_slot;
    return 1;
}

extern "C" int engine_entity_child_count(int slot) {
    if (!active_slot(slot)) return 0;
    int n = 0;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (active_slot(s) && g.entities[s].parentSlot == slot) ++n;
    }
    return n;
}

extern "C" int engine_entity_child(int slot, int index) {
    if (!active_slot(slot) || index < 0) return -1;
    int n = 0;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (active_slot(s) && g.entities[s].parentSlot == slot) {
            if (n == index) return s;
            ++n;
        }
    }
    return -1;
}

/* Flattened depth-first hierarchy for the editor panel. Rebuilt on demand
 * each call (cheap at our entity cap). */
static std::vector<int> g_hierSlots;
static std::vector<int> g_hierDepths;

static void hier_visit(int slot, int depth) {
    if (!active_slot(slot)) return;
    g_hierSlots.push_back(slot);
    g_hierDepths.push_back(depth);
    if (!g.entities[slot].hierExpanded) return;
    for (int c = 0; c < (int)g.entities.size(); ++c) {
        if (active_slot(c) && g.entities[c].parentSlot == slot)
            hier_visit(c, depth + 1);
    }
}

static void hier_rebuild() {
    g_hierSlots.clear();
    g_hierDepths.clear();

    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (!active_slot(s)) continue;
        int p = g.entities[s].parentSlot;
        if (p < 0 || !active_slot(p)) hier_visit(s, 0);
    }
}

extern "C" int engine_entity_hier_count() {
    hier_rebuild();
    return (int)g_hierSlots.size();
}

extern "C" int engine_entity_hier_slot(int row) {
    hier_rebuild();
    if (row < 0 || row >= (int)g_hierSlots.size()) return -1;
    return g_hierSlots[row];
}

extern "C" int engine_entity_hier_depth(int row) {
    hier_rebuild();
    if (row < 0 || row >= (int)g_hierDepths.size()) return 0;
    return g_hierDepths[row];
}

extern "C" int engine_entity_hier_expanded(int slot) {
    if (!active_slot(slot)) return 1;
    return g.entities[slot].hierExpanded ? 1 : 0;
}

extern "C" void engine_entity_set_hier_expanded(int slot, int expanded) {
    if (!active_slot(slot)) return;
    g.entities[slot].hierExpanded = (expanded != 0);
}

extern "C" int engine_entity_toggle_hier_expanded(int slot) {
    if (!active_slot(slot)) return 1;
    g.entities[slot].hierExpanded = !g.entities[slot].hierExpanded;
    return g.entities[slot].hierExpanded ? 1 : 0;
}

extern "C" void engine_entity_hier_collapse_all() {
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (!active_slot(s)) continue;
        if (engine_entity_child_count(s) > 0)
            g.entities[s].hierExpanded = false;
    }
}

extern "C" void engine_entity_hier_expand_all() {
    for (auto& e : g.entities) {
        if (e.active) e.hierExpanded = true;
    }
}

/* ============================================================ */
/*  Scripting context                                           */
/* ============================================================ */

extern "C" int  engine_self() { return g.selfSlot; }
extern "C" void engine_set_self(int s) { g.selfSlot = s; }

/* ============================================================ */
/*  Spatial helpers                                             */
/* ============================================================ */

static int solid_depth(const Entity& e) {
    if (e.depth > 0) return e.depth;
    if (e.kind == "cube" || e.kind == "cylinder") return e.w > 0 ? e.w : 1;
    if (e.kind == "sphere" || e.kind == "capsule") return e.w < e.h ? e.w : e.h;
    if (e.kind == "model") return e.w > e.h ? e.w : e.h;
    return 0;
}

extern "C" int engine_aabb_overlap(int a, int b) {
    if (!active_slot(a) || !active_slot(b)) return 0;
    const Entity& A = g.entities[a];
    const Entity& B = g.entities[b];
    if (A.x + A.w <= B.x) return 0;
    if (B.x + B.w <= A.x) return 0;
    if (A.y + A.h <= B.y) return 0;
    if (B.y + B.h <= A.y) return 0;
    int da = solid_depth(A), db = solid_depth(B);
    if (da > 0 && db > 0) {
        if (A.z + da <= B.z) return 0;
        if (B.z + db <= A.z) return 0;
    }
    return 1;
}

extern "C" int engine_first_overlap(int slot, const char* prefix) {
    if (!active_slot(slot)) return -1;
    std::string p = prefix ? prefix : "";
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (s == slot) continue;
        if (!g.entities[s].active) continue;
        if (!p.empty() && g.entities[s].name.compare(0, p.size(), p) != 0) continue;
        if (engine_aabb_overlap(slot, s)) return s;
    }
    return -1;
}

static bool fps_blocks(const Entity& e) {
    if (!e.active || !e.hasBody || e.bodyType != 0) return false;
    if (e.kind == "camera" || e.kind == "label") return false;
    if (e.kind.compare(0, 5, "light") == 0) return false;
    if (e.kind.compare(0, 2, "ui") == 0) return false;
    return true;
}

static int fps_rot(const Entity& e) {
    int rot = e.rotDeg % 360;
    if (rot < 0) rot += 360;
    if (rot > 180) rot -= 360;
    return rot;
}

static bool fps_surface(const Entity& e, float px, float pz, float& surf) {
    int depth = solid_depth(e);
    if (depth <= 0) return false;
    if (pz < (float)e.z || pz > (float)e.z + depth) return false;
    int rot = fps_rot(e);
    if (rot > -8 && rot < 8) {
        if (px < (float)e.x || px > (float)e.x + e.w) return false;
        surf = (float)e.y;
        return true;
    }
    if (rot > 68 || rot < -68) return false;
    float th = (float)rot * 0.0174532925f;
    float c = std::cos(th), s = std::sin(th);
    if (std::fabs(c) < 0.25f) return false;
    float cx = e.x + e.w * 0.5f;
    float cy = e.y + e.h * 0.5f;
    float hs = e.h * 0.5f;
    float lx = (px - cx - hs * s) / c;
    if (lx < -e.w * 0.5f - 4.f || lx > e.w * 0.5f + 4.f) return false;
    surf = cy + lx * s - hs * c;
    return true;
}

static void fps_push(int self, float& cx, float& cz, float R, float head, float feet) {
    const float step = 42.f;
    for (int pass = 0; pass < 4; ++pass) {
        for (int s = 0; s < (int)g.entities.size(); ++s) {
            if (s == self) continue;
            const Entity& o = g.entities[s];
            if (!fps_blocks(o)) continue;
            int rot = fps_rot(o);
            if (rot <= -8 || rot >= 8) continue;
            float top = (float)o.y;
            float bot = top + o.h;
            if (feet <= top + 1.f) continue;
            if (head >= bot) continue;
            if (feet - top <= step) continue;
            int depth = solid_depth(o);
            if (depth <= 0) continue;
            float sx = (float)o.x, sz = (float)o.z;
            float sw = (float)o.w, sd = (float)depth;
            float qx = cx < sx ? sx : (cx > sx + sw ? sx + sw : cx);
            float qz = cz < sz ? sz : (cz > sz + sd ? sz + sd : cz);
            float dx = cx - qx, dz = cz - qz;
            float d2 = dx * dx + dz * dz;
            if (d2 >= R * R) continue;
            if (d2 < 0.01f) {
                float penL = cx - sx, penR = sx + sw - cx;
                float penB = cz - sz, penF = sz + sd - cz;
                float best = penL;
                int axis = 0;
                if (penR < best) { best = penR; axis = 1; }
                if (penB < best) { best = penB; axis = 2; }
                if (penF < best) { best = penF; axis = 3; }
                if (axis == 0) cx = sx - R;
                else if (axis == 1) cx = sx + sw + R;
                else if (axis == 2) cz = sz - R;
                else cz = sz + sd + R;
            } else {
                float d = std::sqrt(d2);
                float push = (R - d) / d;
                cx += dx * push;
                cz += dz * push;
            }
        }
    }
}

extern "C" int engine_fps_move(int slot, float yaw_deg, float forward, float strafe,
                              int jump, float speed, float jump_speed) {
    if (!active_slot(slot)) return 0;
    Entity& e = g.entities[slot];
    if (e.w < 4) e.w = 4;
    if (e.h < 4) e.h = 4;
    if (e.depth < 4) e.depth = e.w;
    FpsBody& st = g_fps[e.id];
    float dt = (float)g.dtSeconds;
    if (dt < 0.001f) dt = 0.016f;
    if (dt > 0.05f) dt = 0.05f;
    if (forward > 1.f) forward = 1.f;
    if (forward < -1.f) forward = -1.f;
    if (strafe > 1.f) strafe = 1.f;
    if (strafe < -1.f) strafe = -1.f;
    float mag = std::sqrt(forward * forward + strafe * strafe);
    if (mag > 1.f) { forward /= mag; strafe /= mag; }
    float yr = yaw_deg * 0.0174532925f;
    float cyaw = std::cos(yr), syaw = std::sin(yr);
    float dx = (cyaw * forward + syaw * strafe) * speed * dt;
    float dz = (syaw * forward - cyaw * strafe) * speed * dt;

    bool jumping = st.grounded && jump;
    if (jumping) st.vy = -std::fabs(jump_speed);
    else if (!st.grounded) st.vy += 2600.f * dt;
    if (st.vy > 2400.f) st.vy = 2400.f;
    float dy = st.vy * dt;

    float R = std::min(e.w, e.depth) * 0.42f;
    if (R < 10.f) R = 10.f;
    float cx = e.x + e.w * 0.5f + dx;
    float cz = e.z + e.depth * 0.5f + dz;
    fps_push(slot, cx, cz, R, (float)e.y, (float)e.y + e.h);
    e.x = (int)std::lround(cx - e.w * 0.5f);
    e.z = (int)std::lround(cz - e.depth * 0.5f);

    float head0 = (float)e.y;
    float feet0 = head0 + e.h;
    float head1 = head0 + dy;
    for (int s = 0; s < (int)g.entities.size() && dy < -0.5f; ++s) {
        if (s == slot) continue;
        const Entity& o = g.entities[s];
        if (!fps_blocks(o)) continue;
        int depth = solid_depth(o);
        if (depth <= 0) continue;
        if (cx < o.x || cx > o.x + o.w) continue;
        if (cz < o.z || cz > o.z + depth) continue;
        float bot = (float)o.y + o.h;
        if (head0 >= bot && head1 < bot && feet0 > (float)o.y) {
            head1 = bot;
            dy = head1 - head0;
            st.vy = 0.f;
        }
    }
    float feet1 = head1 + e.h;
    if (dy < -0.5f) {
        e.y = (int)std::lround(head1);
        st.grounded = 0;
        return 0;
    }
    const float step = 42.f;
    float lo = feet0 - step;
    float hi = std::max(feet0, feet1) + 6.f;
    bool any = false;
    float best = 0.f;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (s == slot) continue;
        const Entity& o = g.entities[s];
        if (!fps_blocks(o)) continue;
        float surf = 0.f;
        if (!fps_surface(o, cx, cz, surf)) continue;
        if (surf < lo || surf > hi) continue;
        if (!any || surf < best) { best = surf; any = true; }
    }
    if (any) {
        e.y = (int)std::lround(best - e.h);
        st.vy = 0.f;
        st.grounded = 1;
        return 1;
    }
    e.y = (int)std::lround(head1);
    st.grounded = 0;
    return 0;
}

/* ============================================================ */
/*  Text editor buffer                                          */
/* ============================================================ */

namespace {

void te_clamp_cursor() {
    if (g.teLines.empty()) g.teLines.push_back("");
    if (g.teCursorLine < 0) g.teCursorLine = 0;
    if (g.teCursorLine >= (int)g.teLines.size()) g.teCursorLine = (int)g.teLines.size() - 1;
    int n = (int)g.teLines[g.teCursorLine].size();
    if (g.teCursorCol < 0)  g.teCursorCol = 0;
    if (g.teCursorCol > n)  g.teCursorCol = n;
}

} // namespace

extern "C" int engine_te_load(const char* path) {
    if (!path) return 0;
    g.teLines.clear();
    g.teCursorLine = 0;
    g.teCursorCol  = 0;
    g.teScrollY    = 0;
    g.teDirty      = false;
    g.tePath       = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        g.teLines.push_back("");
        return 0;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        g.teLines.push_back(line);
    }
    if (g.teLines.empty()) g.teLines.push_back("");
    return 1;
}

extern "C" int engine_te_save() {
    if (g.tePath.empty()) return 0;
    std::ofstream f(g.tePath, std::ios::binary);
    if (!f) return 0;
    for (size_t i = 0; i < g.teLines.size(); ++i) {
        f << g.teLines[i];
        if (i + 1 < g.teLines.size()) f << "\n";
    }
    g.teDirty = false;
    return 1;
}

extern "C" int engine_te_save_as(const char* path) {
    if (!path) return 0;
    g.tePath = path;
    return engine_te_save();
}

extern "C" void engine_te_clear() {
    g.teLines.clear();
    g.teLines.push_back("");
    g.teCursorLine = 0;
    g.teCursorCol  = 0;
    g.teScrollY    = 0;
    g.teDirty      = false;
    g.tePath.clear();
}

extern "C" const char* engine_te_path() { return g.tePath.c_str(); }
extern "C" int         engine_te_dirty() { return g.teDirty ? 1 : 0; }
extern "C" int         engine_te_line_count() { return (int)g.teLines.size(); }
extern "C" const char* engine_te_line(int i) {
    if (i < 0 || i >= (int)g.teLines.size()) return "";
    return g.teLines[i].c_str();
}
extern "C" int  engine_te_cursor_line() { return g.teCursorLine; }
extern "C" int  engine_te_cursor_col()  { return g.teCursorCol; }
extern "C" void engine_te_set_cursor(int line, int col) {
    g.teCursorLine = line;
    g.teCursorCol  = col;
    te_clamp_cursor();
}
extern "C" void engine_te_move_cursor(int dl, int dc) {
    g.teCursorLine += dl;
    g.teCursorCol  += dc;
    if (g.teCursorLine < 0) g.teCursorLine = 0;
    if (g.teCursorLine >= (int)g.teLines.size()) g.teCursorLine = (int)g.teLines.size() - 1;
    /* allow jumping across lines on horizontal motion */
    if (g.teCursorCol < 0) {
        if (g.teCursorLine > 0) {
            g.teCursorLine--;
            g.teCursorCol = (int)g.teLines[g.teCursorLine].size();
        } else {
            g.teCursorCol = 0;
        }
    } else if (g.teCursorCol > (int)g.teLines[g.teCursorLine].size()) {
        if (g.teCursorLine + 1 < (int)g.teLines.size()) {
            g.teCursorLine++;
            g.teCursorCol = 0;
        } else {
            g.teCursorCol = (int)g.teLines[g.teCursorLine].size();
        }
    }
}
extern "C" void engine_te_home() { te_clamp_cursor(); g.teCursorCol = 0; }
extern "C" void engine_te_end()  {
    te_clamp_cursor();
    g.teCursorCol = (int)g.teLines[g.teCursorLine].size();
}
extern "C" void engine_te_page(int dir) {
    g.teCursorLine += dir;
    if (g.teCursorLine < 0) g.teCursorLine = 0;
    if (g.teCursorLine >= (int)g.teLines.size()) g.teCursorLine = (int)g.teLines.size() - 1;
    te_clamp_cursor();
}

extern "C" void engine_te_insert_char(int c) {
    if (c == '\r' || c == '\n') { engine_te_newline(); return; }
    if (c < 0) return;
    te_clamp_cursor();
    char ch = (char)c;
    g.teLines[g.teCursorLine].insert(g.teCursorCol, 1, ch);
    g.teCursorCol += 1;
    g.teDirty = true;
}
extern "C" void engine_te_insert_text(const char* s) {
    if (!s) return;
    while (*s) { engine_te_insert_char((unsigned char)*s++); }
}
extern "C" void engine_te_newline() {
    te_clamp_cursor();
    std::string& cur = g.teLines[g.teCursorLine];
    std::string tail = cur.substr(g.teCursorCol);
    cur.erase(g.teCursorCol);
    /* auto indent: copy leading spaces of current line into the new line */
    std::string indent;
    for (char ch : cur) {
        if (ch == ' ' || ch == '\t') indent.push_back(ch);
        else break;
    }
    g.teLines.insert(g.teLines.begin() + g.teCursorLine + 1, indent + tail);
    g.teCursorLine += 1;
    g.teCursorCol = (int)indent.size();
    g.teDirty = true;
}
extern "C" void engine_te_backspace() {
    te_clamp_cursor();
    if (g.teCursorCol > 0) {
        g.teLines[g.teCursorLine].erase(g.teCursorCol - 1, 1);
        g.teCursorCol -= 1;
        g.teDirty = true;
    } else if (g.teCursorLine > 0) {
        std::string cur = g.teLines[g.teCursorLine];
        g.teLines.erase(g.teLines.begin() + g.teCursorLine);
        g.teCursorLine -= 1;
        g.teCursorCol = (int)g.teLines[g.teCursorLine].size();
        g.teLines[g.teCursorLine] += cur;
        g.teDirty = true;
    }
}
extern "C" void engine_te_delete() {
    te_clamp_cursor();
    int n = (int)g.teLines[g.teCursorLine].size();
    if (g.teCursorCol < n) {
        g.teLines[g.teCursorLine].erase(g.teCursorCol, 1);
        g.teDirty = true;
    } else if (g.teCursorLine + 1 < (int)g.teLines.size()) {
        g.teLines[g.teCursorLine] += g.teLines[g.teCursorLine + 1];
        g.teLines.erase(g.teLines.begin() + g.teCursorLine + 1);
        g.teDirty = true;
    }
}
extern "C" int  engine_te_scroll_y() { return g.teScrollY; }
extern "C" void engine_te_set_scroll_y(int v) {
    if (v < 0) v = 0;
    g.teScrollY = v;
}

/* ============================================================ */
/*  Selection                                                   */
/* ============================================================ */

extern "C" int engine_entity_selected(int slot) {
    return active_slot(slot) && g.entities[slot].selected ? 1 : 0;
}
extern "C" void engine_entity_set_selected(int slot, int v) {
    if (active_slot(slot)) g.entities[slot].selected = (v != 0);
}
extern "C" void engine_clear_selection() {
    for (auto& e : g.entities) e.selected = false;
}
extern "C" int engine_selection_count() {
    int n = 0;
    for (auto& e : g.entities) if (e.active && e.selected) ++n;
    return n;
}
extern "C" int engine_selection_first() {
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (g.entities[s].active && g.entities[s].selected) return s;
    }
    return -1;
}
extern "C" int engine_selection_iter_next(int from) {
    if (from < 0) from = 0;
    for (int s = from; s < (int)g.entities.size(); ++s) {
        if (g.entities[s].active && g.entities[s].selected) return s;
    }
    return -1;
}

/* ============================================================ */
/*  Undo / redo                                                 */
/* ============================================================ */

namespace {

constexpr int kUndoCap = 256;

UndoEntry make_snap(int slot) {
    UndoEntry e{};
    e.slot = slot;
    e.group = g.undoGroup;
    if (active_slot(slot)) e.snap = g.entities[slot];
    return e;
}

/* How many entries at the top of the stack belong to the latest step. */
int undo_step_count(const std::vector<UndoEntry>& stack) {
    if (stack.empty()) return 0;
    int group = stack.back().group;
    if (group == 0) return 1;
    int n = 0;
    for (int i = (int)stack.size() - 1; i >= 0; --i) {
        if (stack[i].group != group) break;
        ++n;
    }
    return n;
}

void undo_push_back(std::vector<UndoEntry>& stack, UndoEntry e) {
    stack.push_back(std::move(e));
    if ((int)stack.size() > kUndoCap) stack.erase(stack.begin());
}

void apply_snap_inplace(int slot, const Entity& snap) {
    if (slot < 0 || slot >= (int)g.entities.size()) return;
    Entity& e = g.entities[slot];
    if (e.spriteId != 0) { engine_image_free(e.spriteId); e.spriteId = 0; }
    e = snap;
    if (!e.sprite.empty()) {
        std::string p = asset_resolve(e.sprite);
        e.spriteId = engine_image_load(p.c_str());
    } else {
        e.spriteId = 0;
    }
}

} // namespace

extern "C" void engine_undo_begin_group() {
    if (g.undoGroup != 0) return;
    g.undoGroup = g.undoGroupGen++;
    if (g.undoGroup == 0) g.undoGroup = g.undoGroupGen++;
}
extern "C" void engine_undo_end_group() {
    g.undoGroup = 0;
}

extern "C" void engine_undo_save_modify(int slot) {
    if (!active_slot(slot)) return;
    UndoEntry e = make_snap(slot);
    e.op = 0;
    undo_push_back(g.undoStack, std::move(e));
    g.redoStack.clear();
}

extern "C" void engine_undo_save_create(int slot) {
    if (!active_slot(slot)) return;
    UndoEntry e = make_snap(slot);
    e.op = 1;
    undo_push_back(g.undoStack, std::move(e));
    g.redoStack.clear();
}

extern "C" void engine_undo_save_delete(int slot) {
    if (!active_slot(slot)) return;
    UndoEntry e = make_snap(slot);
    e.op = 2;
    undo_push_back(g.undoStack, std::move(e));
    g.redoStack.clear();
}

extern "C" int engine_undo() {
    int n = undo_step_count(g.undoStack);
    if (n <= 0) return 0;
    for (int i = 0; i < n; ++i) {
        UndoEntry top = std::move(g.undoStack.back());
        g.undoStack.pop_back();
        UndoEntry redo = top;
        if (top.op == 0) {            /* modify: swap snapshot */
            if (active_slot(top.slot)) redo.snap = g.entities[top.slot];
            apply_snap_inplace(top.slot, top.snap);
        } else if (top.op == 1) {     /* create: undo destroys, snapshot already captured */
            if (active_slot(top.slot)) redo.snap = g.entities[top.slot];
            engine_entity_destroy(top.slot);
        } else if (top.op == 2) {     /* delete: undo restores */
            apply_snap_inplace(top.slot, top.snap);
        }
        g.redoStack.push_back(std::move(redo));
        if ((int)g.redoStack.size() > kUndoCap) g.redoStack.erase(g.redoStack.begin());
    }
    return 1;
}

extern "C" int engine_redo() {
    int n = undo_step_count(g.redoStack);
    if (n <= 0) return 0;
    for (int i = 0; i < n; ++i) {
        UndoEntry top = std::move(g.redoStack.back());
        g.redoStack.pop_back();
        UndoEntry undo = top;
        if (top.op == 0) {
            if (active_slot(top.slot)) undo.snap = g.entities[top.slot];
            apply_snap_inplace(top.slot, top.snap);
        } else if (top.op == 1) {     /* redoing create: restore snapshot */
            apply_snap_inplace(top.slot, top.snap);
        } else if (top.op == 2) {     /* redoing delete: destroy */
            if (active_slot(top.slot)) undo.snap = g.entities[top.slot];
            engine_entity_destroy(top.slot);
        }
        g.undoStack.push_back(std::move(undo));
        if ((int)g.undoStack.size() > kUndoCap) g.undoStack.erase(g.undoStack.begin());
    }
    return 1;
}

extern "C" void engine_undo_clear() {
    g.undoStack.clear();
    g.redoStack.clear();
    g.undoGroup = 0;
}
extern "C" void engine_undo_trim(int keep) {
    if (keep < 0) keep = 0;
    if ((int)g.undoStack.size() > keep) g.undoStack.resize((size_t)keep);
    g.redoStack.clear();
    g.undoGroup = 0;
}
extern "C" int engine_undo_depth() { return (int)g.undoStack.size(); }
extern "C" int engine_redo_depth() { return (int)g.redoStack.size(); }

extern "C" int engine_hit_at(int x, int y) {
    for (int s = (int)g.entities.size() - 1; s >= 0; --s) {
        const Entity& e = g.entities[s];
        if (!e.active) continue;
        if (x < e.x || y < e.y) continue;
        if (x >= e.x + e.w || y >= e.y + e.h) continue;
        return s;
    }
    return -1;
}

/* ============================================================ */
/*  Console / log                                               */
/* ============================================================ */

extern "C" void engine_log(const char* msg) {
    if (msg) log_push(msg);
}
extern "C" int engine_log_count() { return (int)g.log.size(); }
extern "C" const char* engine_log_get(int i) {
    if (i < 0 || i >= (int)g.log.size()) return "";
    return g.log[i].c_str();
}
extern "C" void engine_log_clear() { g.log.clear(); }

/* ============================================================ */
/*  Scene serialization                                         */
/* ============================================================ */

namespace {

std::vector<std::string> split_pipe(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '|') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

} // namespace

/* ============================================================ */
/*  Physics (Box2D)                                             */
/* ============================================================ */

namespace {

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;

b2BodyType box2d_type(int t) {
    if (t == 0) return b2_staticBody;
    if (t == 2) return b2_kinematicBody;
    return b2_dynamicBody;
}

struct HitPiece {
    float cx, cy, hw, hh;                 /* screen px from the entity center, for physics */
    float x0, y0, z0, x1, y1, z1;         /* model space, same fit as the drawn mesh */
};

struct HitCache {
    int id = -1;
    int acc = 0;
    int w = 0;
    int h = 0;
    std::string model;
    std::vector<HitPiece> pieces;
    std::vector<float> skin; /* 9 floats per triangle, model space, the cover */
};
std::unordered_map<int, HitCache> g_hitCache;

int hitbox_cells(int acc) {
    if (acc <= 1) return 4;
    if (acc == 2) return 6;
    if (acc == 3) return 8;
    if (acc == 4) return 12;
    return 16;
}

/* Vertices in the same centered unit space the 3D view uses for a model. */
bool model_local(const Entity& e, std::vector<float>& xyz, std::vector<int>& tris) {
    if (e.model.empty()) return false;
    std::ifstream f(asset_resolve(e.model));
    if (!f) return false;
    std::vector<float> raw;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() > 2 && line[0] == 'v' && line[1] == ' ') {
            float x = 0, y = 0, z = 0;
            if (sscanf(line.c_str() + 2, "%f %f %f", &x, &y, &z) == 3) {
                raw.push_back(x); raw.push_back(y); raw.push_back(z);
            }
        } else if (line.size() > 2 && line[0] == 'f' && line[1] == ' ') {
            std::istringstream ss(line.substr(2));
            std::string tok;
            std::vector<int> face;
            int nraw = (int)raw.size() / 3;
            while (ss >> tok) {
                int idx = std::atoi(tok.c_str());
                if (idx < 0) idx = nraw + idx; else idx -= 1;
                if (idx >= 0) face.push_back(idx);
            }
            for (size_t k = 1; k + 1 < face.size(); ++k) {
                tris.push_back(face[0]);
                tris.push_back((int)face[k]);
                tris.push_back((int)face[k + 1]);
            }
        }
    }
    if (raw.size() < 9 || tris.empty()) return false;
    float minx = raw[0], miny = raw[1], minz = raw[2];
    float maxx = minx, maxy = miny, maxz = minz;
    for (size_t i = 0; i + 2 < raw.size(); i += 3) {
        minx = std::min(minx, raw[i]); maxx = std::max(maxx, raw[i]);
        miny = std::min(miny, raw[i + 1]); maxy = std::max(maxy, raw[i + 1]);
        minz = std::min(minz, raw[i + 2]); maxz = std::max(maxz, raw[i + 2]);
    }
    float ext = std::max(maxx - minx, std::max(maxy - miny, maxz - minz));
    if (ext <= 0.f) ext = 1.f;
    float midx = (minx + maxx) * 0.5f;
    float midy = (miny + maxy) * 0.5f;
    float midz = (minz + maxz) * 0.5f;
    xyz.resize(raw.size());
    for (size_t i = 0; i + 2 < raw.size(); i += 3) {
        xyz[i]     = (raw[i]     - midx) / ext;
        xyz[i + 1] = (raw[i + 1] - midy) / ext;
        xyz[i + 2] = (raw[i + 2] - midz) / ext;
    }
    return true;
}

const HitCache& hitbox_cache(const Entity& e) {
    static const HitCache none;
    if (e.hitbox <= 0 || e.kind != "model") return none;
    auto found = g_hitCache.find(e.id);
    if (found != g_hitCache.end()) {
        const HitCache& c = found->second;
        if (c.id == e.id && c.acc == e.hitbox && c.w == e.w && c.h == e.h && c.model == e.model)
            return c;
    }
    HitCache built;
    built.id = e.id;
    built.acc = e.hitbox;
    built.w = e.w;
    built.h = e.h;
    built.model = e.model;
    std::vector<float> xyz;
    std::vector<int> tris;
    if (model_local(e, xyz, tris)) {
        int n = hitbox_cells(e.hitbox);
        float size = (float)std::max(e.w, e.h);
        if (size < 4.f) size = 4.f;
        float bminx = 1e9f, bminy = 1e9f, bminz = 1e9f;
        float bmaxx = -1e9f, bmaxy = -1e9f, bmaxz = -1e9f;
        for (size_t vi = 0; vi + 2 < xyz.size(); vi += 3) {
            bminx = std::min(bminx, xyz[vi]); bmaxx = std::max(bmaxx, xyz[vi]);
            bminy = std::min(bminy, xyz[vi + 1]); bmaxy = std::max(bmaxy, xyz[vi + 1]);
            bminz = std::min(bminz, xyz[vi + 2]); bmaxz = std::max(bmaxz, xyz[vi + 2]);
        }
        /* Skin thickness follows the thin axis. A long hall stays a shell:
           the floor and walls are solid, and the air between them is not. */
        /* A hall is long into the screen. Flattening it stacks the whole
           length into one slab and the player is pushed out through the floor.
           Those meshes are solved in 3D from the real triangles. */
        bool walk3d = (bmaxz - bminz) > (bmaxy - bminy) * 1.25f;
        float cross = std::min(bmaxx - bminx, std::min(bmaxy - bminy, bmaxz - bminz)) * size;
        if (cross < 8.f) cross = 8.f;
        float thick = cross / (float)n;
        if (thick < 6.f) thick = 6.f;
        if (thick > cross * 0.3f) thick = cross * 0.3f;
        struct Span { float a, b; };
        struct Band { std::vector<Span> spans; double acc = 0; int count = 0; };
        std::unordered_map<int, Band> floors, walls;
        auto add_span = [&](Band& band, float a, float b, float center) {
            if (b < a) std::swap(a, b);
            if (b - a < 0.5f) return;
            band.spans.push_back(Span{a, b});
            band.acc += center;
            band.count += 1;
        };
        auto y_on_slice = [&](float lo, float hi, float ax, float ay, float bx, float by,
                              float cx, float cy, float& ylo, float& yhi) {
            ylo = 1e9f;
            yhi = -1e9f;
            auto edge = [&](float xA, float yA, float xB, float yB) {
                if (xA > xB) { std::swap(xA, xB); std::swap(yA, yB); }
                float a = std::max(lo, xA);
                float b = std::min(hi, xB);
                if (b < a - 0.01f) return;
                float denom = xB - xA;
                float t0 = std::fabs(denom) < 1e-6f ? 0.f : (a - xA) / denom;
                float t1 = std::fabs(denom) < 1e-6f ? 1.f : (b - xA) / denom;
                float ya = yA + (yB - yA) * t0;
                float yb = yA + (yB - yA) * t1;
                ylo = std::min(ylo, std::min(ya, yb));
                yhi = std::max(yhi, std::max(ya, yb));
            };
            edge(ax, ay, bx, by);
            edge(bx, by, cx, cy);
            edge(cx, cy, ax, ay);
            return yhi >= ylo;
        };
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            int ia = tris[t], ib = tris[t + 1], ic = tris[t + 2];
            if (ia < 0 || ib < 0 || ic < 0) continue;
            if ((ia + 1) * 3 > (int)xyz.size() || (ib + 1) * 3 > (int)xyz.size() || (ic + 1) * 3 > (int)xyz.size()) continue;
            float ax = xyz[ia * 3], ay = xyz[ia * 3 + 1], az = xyz[ia * 3 + 2];
            float bx = xyz[ib * 3], by = xyz[ib * 3 + 1], bz = xyz[ib * 3 + 2];
            float cx = xyz[ic * 3], cy = xyz[ic * 3 + 1], cz = xyz[ic * 3 + 2];
            float ex = (by - ay) * (cz - az) - (bz - az) * (cy - ay);
            float ey = (bz - az) * (cx - ax) - (bx - ax) * (cz - az);
            float ez = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            float el2 = ex * ex + ey * ey + ez * ez;
            if (el2 < 1e-12f) continue;
            if (walk3d) {
                built.skin.push_back(ax * size); built.skin.push_back(-ay * size); built.skin.push_back(-az * size);
                built.skin.push_back(bx * size); built.skin.push_back(-by * size); built.skin.push_back(-bz * size);
                built.skin.push_back(cx * size); built.skin.push_back(-cy * size); built.skin.push_back(-cz * size);
                continue;
            }
            bool yFace = std::fabs(ey) >= std::fabs(ex) && std::fabs(ey) >= std::fabs(ez);
            bool xFace = !yFace && std::fabs(ex) >= std::fabs(ez);
            /* A face looking down the hall flattens across the whole opening. */
            if (!yFace && !xFace) continue;
            /* model +Y is up; the physics plane's +Y is down the screen */
            float sax = ax * size, say = -ay * size;
            float sbx = bx * size, sby = -by * size;
            float scx = cx * size, scy = -cy * size;
            if (yFace) {
                float x0 = std::min(sax, std::min(sbx, scx));
                float x1 = std::max(sax, std::max(sbx, scx));
                float y0 = std::min(say, std::min(sby, scy));
                float y1 = std::max(say, std::max(sby, scy));
                int slices = 1;
                if (y1 - y0 > thick * 1.5f) {
                    slices = (int)std::ceil((x1 - x0) / std::max(thick, 1.f));
                    if (slices < 1) slices = 1;
                    if (slices > n * 2) slices = n * 2;
                }
                float nudge = (ey > 0.f ? 1.f : -1.f) * thick * 0.5f;
                for (int s = 0; s < slices; ++s) {
                    float xa = x0 + (x1 - x0) * ((float)s / (float)slices);
                    float xb = x0 + (x1 - x0) * ((float)(s + 1) / (float)slices);
                    float ylo = 0.f, yhi = 0.f;
                    if (!y_on_slice(xa, xb, sax, say, sbx, sby, scx, scy, ylo, yhi)) continue;
                    float surf = (ylo + yhi) * 0.5f;
                    float center = surf + nudge;
                    int key = (int)std::lround(center / thick);
                    add_span(floors[key], xa, xb, center);
                }
            } else {
                float y0 = std::min(say, std::min(sby, scy));
                float y1 = std::max(say, std::max(sby, scy));
                float x0 = std::min(sax, std::min(sbx, scx));
                float x1 = std::max(sax, std::max(sbx, scx));
                int slices = 1;
                if (x1 - x0 > thick * 1.5f) {
                    slices = (int)std::ceil((y1 - y0) / std::max(thick, 1.f));
                    if (slices < 1) slices = 1;
                    if (slices > n * 2) slices = n * 2;
                }
                float nudge = (ex > 0.f ? -1.f : 1.f) * thick * 0.5f;
                for (int s = 0; s < slices; ++s) {
                    float ya = y0 + (y1 - y0) * ((float)s / (float)slices);
                    float yb = y0 + (y1 - y0) * ((float)(s + 1) / (float)slices);
                    float xlo = 0.f, xhi = 0.f;
                    if (!y_on_slice(ya, yb, say, sax, sby, sbx, scy, scx, xlo, xhi)) continue;
                    float surf = (xlo + xhi) * 0.5f;
                    float center = surf + nudge;
                    int key = (int)std::lround(center / thick);
                    add_span(walls[key], ya, yb, center);
                }
            }
        }
        auto flush = [&](std::unordered_map<int, Band>& bands, bool horizontal) {
            for (auto& kv : bands) {
                auto& spans = kv.second.spans;
                if (spans.empty() || kv.second.count <= 0) continue;
                std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.a < b.a; });
                float fixed = (float)(kv.second.acc / (double)kv.second.count);
                float ca = spans[0].a, cb = spans[0].b;
                auto emit = [&](float a, float b) {
                    HitPiece p;
                    if (horizontal) {
                        p.cx = (a + b) * 0.5f;
                        p.cy = fixed;
                        p.hw = (b - a) * 0.5f;
                        p.hh = thick * 0.5f;
                    } else {
                        p.cx = fixed;
                        p.cy = (a + b) * 0.5f;
                        p.hw = thick * 0.5f;
                        p.hh = (b - a) * 0.5f;
                    }
                    if (p.hw < 0.5f) p.hw = 0.5f;
                    if (p.hh < 0.5f) p.hh = 0.5f;
                    p.x0 = p.y0 = p.z0 = p.x1 = p.y1 = p.z1 = 0.f;
                    built.pieces.push_back(p);
                };
                for (size_t i = 1; i < spans.size(); ++i) {
                    if (spans[i].a <= cb + thick) cb = std::max(cb, spans[i].b);
                    else { emit(ca, cb); ca = spans[i].a; cb = spans[i].b; }
                }
                emit(ca, cb);
            }
        };
        if (!walk3d) {
            flush(floors, true);
            flush(walls, false);
        }
        if (!walk3d && built.pieces.empty()) {
            float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
            for (size_t vi = 0; vi + 2 < xyz.size(); vi += 3) {
                float sx = xyz[vi] * size, sy = -xyz[vi + 1] * size;
                minx = std::min(minx, sx); maxx = std::max(maxx, sx);
                miny = std::min(miny, sy); maxy = std::max(maxy, sy);
            }
            if (maxx > minx && maxy > miny) {
                HitPiece p;
                p.cx = (minx + maxx) * 0.5f;
                p.cy = (miny + maxy) * 0.5f;
                p.hw = (maxx - minx) * 0.5f;
                p.hh = (maxy - miny) * 0.5f;
                if (p.hw < 0.5f) p.hw = 0.5f;
                if (p.hh < 0.5f) p.hh = 0.5f;
                p.x0 = p.y0 = p.z0 = p.x1 = p.y1 = p.z1 = 0.f;
                built.pieces.push_back(p);
            }
        }
    }
    g_hitCache[e.id] = std::move(built);
    return g_hitCache[e.id];
}

const std::vector<HitPiece>& hitbox_pieces(const Entity& e) {
    return hitbox_cache(e).pieces;
}

const std::vector<float>& hitbox_skin(const Entity& e) {
    return hitbox_cache(e).skin;
}

void physics_create_body(Entity& e) {
    if (!g.physWorld) return;
    float ppm = g.pixelsPerMeter;
    int w = e.w < 4 ? 4 : e.w;
    int h = e.h < 4 ? 4 : e.h;
    float cx = (e.x + w / 2.0f) / ppm;
    float cy = (e.y + h / 2.0f) / ppm;

    b2BodyDef bd;
    bd.type = box2d_type(e.bodyType);
    bd.position.Set(cx, cy);
    /* Screen +Y is down, so a positive Box2D angle is clockwise on screen,
       the same direction GDI and the 3D view use for rotDeg. Negating it
       mirrored the collider: a ball fell through the drawn slope and was
       shoved out the opposite side. */
    bd.angle = e.rotDeg * kDeg2Rad;
    bd.fixedRotation = e.fixedRotation;
    bd.gravityScale = e.gravityScale;
    b2Body* body = g.physWorld->CreateBody(&bd);

    b2FixtureDef fd;
    fd.density = e.density > 0.0f ? e.density : 1.0f;
    fd.friction = e.friction;
    fd.restitution = e.restitution;
    b2PolygonShape box;
    b2CircleShape circle;
    const std::vector<HitPiece>* mesh = nullptr;
    bool shell = false;
    if (e.hitbox > 0 && e.kind == "model") {
        const HitCache& cache = hitbox_cache(e);
        if (!cache.pieces.empty()) mesh = &cache.pieces;
        else if (!cache.skin.empty()) shell = true;
    }
    if (mesh) {
        for (const HitPiece& p : *mesh) {
            b2PolygonShape poly;
            float hx = p.hw / ppm;
            float hy = p.hh / ppm;
            if (hx < 0.01f) hx = 0.01f;
            if (hy < 0.01f) hy = 0.01f;
            poly.SetAsBox(hx, hy, b2Vec2(p.cx / ppm, p.cy / ppm), 0.f);
            fd.shape = &poly;
            body->CreateFixture(&fd);
        }
    } else if (shell) {
        /* The triangles are tested in 3D after the step. A flat box here
           would seal the hallway. */
    } else if (e.kind == "sphere") {
        float rad = (float)(w < h ? w : h) * 0.5f / ppm;
        if (rad < 0.05f) rad = 0.05f;
        circle.m_radius = rad;
        fd.shape = &circle;
        body->CreateFixture(&fd);
    } else if (e.kind == "capsule") {
        /* Front view of the upright capsule: a stadium. Ends are circles. */
        float rad = (float)(w < h ? w : h) * 0.5f / ppm;
        if (rad < 0.05f) rad = 0.05f;
        if (h <= w) {
            circle.m_radius = rad;
            fd.shape = &circle;
            body->CreateFixture(&fd);
        } else {
            float bodyH = (h - w) * 0.5f / ppm;
            if (bodyH < 0.01f) bodyH = 0.01f;
            box.SetAsBox((w * 0.5f) / ppm, bodyH);
            fd.shape = &box;
            body->CreateFixture(&fd);
            circle.m_radius = (w * 0.5f) / ppm;
            if (circle.m_radius < 0.05f) circle.m_radius = 0.05f;
            circle.m_p.Set(0.f, bodyH);
            fd.shape = &circle;
            body->CreateFixture(&fd);
            circle.m_p.Set(0.f, -bodyH);
            fd.shape = &circle;
            body->CreateFixture(&fd);
        }
    } else {
        box.SetAsBox((w / 2.0f) / ppm, (h / 2.0f) / ppm);
        fd.shape = &box;
        body->CreateFixture(&fd);
    }

    e.body = body;
}

void physics_rebuild_body(Entity& e) {
    if (!g.physWorld) return;
    if (e.body) {
        g.physWorld->DestroyBody(e.body);
        e.body = nullptr;
    }
    if (e.active && e.hasBody) physics_create_body(e);
}

} // namespace

/* A script teleport has to move the rigid body too. Otherwise the next
   physics step copies the old body position back over x, y, and rotation. */
static void physics_apply_entity(Entity& e) {
    if (g_physWriteback) return;
    if (!g.physWorld || !e.hasBody || !e.body) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    int w = e.w < 4 ? 4 : e.w;
    int h = e.h < 4 ? 4 : e.h;
    float cx = (e.x + w / 2.0f) / ppm;
    float cy = (e.y + h / 2.0f) / ppm;
    float ang = e.rotDeg * (3.14159265358979323846f / 180.0f);
    e.body->SetTransform(b2Vec2(cx, cy), ang);
    e.body->SetLinearVelocity(b2Vec2(0.f, 0.f));
    e.body->SetAngularVelocity(0.f);
    e.body->SetAwake(true);
}

extern "C" void engine_physics_set_gravity(float gx, float gy) {
    g.gravityX = gx;
    g.gravityY = gy;
    if (g.physWorld) {
        g.physWorld->SetGravity(b2Vec2(gx / g.pixelsPerMeter, gy / g.pixelsPerMeter));
    }
}
extern "C" float engine_physics_gravity_x() { return g.gravityX; }
extern "C" float engine_physics_gravity_y() { return g.gravityY; }
extern "C" float engine_physics_ppm() { return g.pixelsPerMeter; }
extern "C" void  engine_physics_set_ppm(float ppm) {
    if (ppm < 1.0f) ppm = 1.0f;
    if (ppm > 400.0f) ppm = 400.0f;
    g.pixelsPerMeter = ppm;
}
extern "C" int   engine_physics_active() { return g.physWorld ? 1 : 0; }

extern "C" void engine_physics_begin() {
    if (g.physWorld) engine_physics_end();
    g.physWorld = new b2World(b2Vec2(g.gravityX / g.pixelsPerMeter,
                                     g.gravityY / g.pixelsPerMeter));
    for (Entity& e : g.entities) {
        e.body = nullptr;
        if (e.active && e.hasBody) physics_create_body(e);
    }
}

/* Keep a moving body on a mesh shell: stand on the floor under its feet
   and slide out of walls. The shell is stored as pixel offsets from the
   model center, with scene Y down and scene Z into the screen. */
static void mesh_hold(int slot) {
    if (!active_slot(slot)) return;
    Entity& m = g.entities[slot];
    if (m.bodyType == 0 || !m.body) return;
    int depth = solid_depth(m);
    if (depth < 4) depth = m.w > 0 ? m.w : 4;
    float R = std::min((float)m.w, (float)depth) * 0.45f;
    if (R < 8.f) R = 8.f;
    /* The look camera sits 42px in front of the body. A tighter radius lets
       the eyes pass through a wall and see outside. */
    bool eyes = m.text == "look";
    if (!eyes) {
        for (const Entity& c : g.entities) {
            if (c.active && c.parentSlot == slot && c.text == "look") { eyes = true; break; }
        }
    }
    if (eyes && R < 56.f) R = 56.f;
    float px = m.x + m.w * 0.5f;
    float pz = m.z + depth * 0.5f;
    auto in_tri = [](float x, float y, float ax, float ay, float bx, float by, float cx, float cy) {
        float c1 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
        float c2 = (cx - bx) * (y - by) - (cy - by) * (x - bx);
        float c3 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
        bool neg = c1 < -0.5f || c2 < -0.5f || c3 < -0.5f;
        bool pos = c1 > 0.5f || c2 > 0.5f || c3 > 0.5f;
        return !(neg && pos);
    };
    for (int pass = 0; pass < 3; ++pass) {
        float head = (float)m.y;
        float feet = head + m.h;
        for (int s = 0; s < (int)g.entities.size(); ++s) {
            if (s == slot) continue;
            const Entity& o = g.entities[s];
            if (!o.active || !o.hasBody || o.bodyType != 0) continue;
            if (o.kind != "model" || o.hitbox <= 0) continue;
            const std::vector<float>& skin = hitbox_cache(o).skin;
            if (skin.size() < 9) continue;
            int od = solid_depth(o);
            float ocx = o.x + o.w * 0.5f;
            float ocy = o.y + o.h * 0.5f;
            float ocz = o.z + od * 0.5f;
            for (size_t i = 0; i + 8 < skin.size(); i += 9) {
                float ax = ocx + skin[i],     ay = ocy + skin[i + 1], az = ocz + skin[i + 2];
                float bx = ocx + skin[i + 3], by = ocy + skin[i + 4], bz = ocz + skin[i + 5];
                float cx = ocx + skin[i + 6], cy = ocy + skin[i + 7], cz = ocz + skin[i + 8];
                float minx = std::min(ax, std::min(bx, cx)) - R;
                float maxx = std::max(ax, std::max(bx, cx)) + R;
                float minz = std::min(az, std::min(bz, cz)) - R;
                float maxz = std::max(az, std::max(bz, cz)) + R;
                if (px < minx || px > maxx || pz < minz || pz > maxz) continue;
                float ex = (by - ay) * (cz - az) - (bz - az) * (cy - ay);
                float ey = (bz - az) * (cx - ax) - (bx - ax) * (cz - az);
                float ez = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                float el2 = ex * ex + ey * ey + ez * ez;
                if (el2 < 1.f) continue;
                if (std::fabs(ey) >= std::fabs(ex) && std::fabs(ey) >= std::fabs(ez)) continue;
                float y0 = std::min(ay, std::min(by, cy));
                float y1 = std::max(ay, std::max(by, cy));
                if (feet < y0 || head > y1) continue;
                float qx = px, qz = pz;
                float best = 1e30f;
                auto seg = [&](float xA, float zA, float xB, float zB) {
                    float abx = xB - xA, abz = zB - zA;
                    float apx = px - xA, apz = pz - zA;
                    float ab2 = abx * abx + abz * abz;
                    float t = ab2 > 1e-6f ? (apx * abx + apz * abz) / ab2 : 0.f;
                    if (t < 0.f) t = 0.f;
                    if (t > 1.f) t = 1.f;
                    float sx = xA + abx * t, sz = zA + abz * t;
                    float dx = px - sx, dz = pz - sz;
                    float d2 = dx * dx + dz * dz;
                    if (d2 < best) { best = d2; qx = sx; qz = sz; }
                };
                seg(ax, az, bx, bz);
                seg(bx, bz, cx, cz);
                seg(cx, cz, ax, az);
                if (best >= R * R) continue;
                float dx = px - qx, dz = pz - qz;
                float d = std::sqrt(best);
                if (d > 0.5f) {
                    float push = (R - d) / d;
                    px += dx * push;
                    pz += dz * push;
                } else {
                    float el = std::sqrt(el2);
                    float nx = ex / el, nz = ez / el;
                    float mx3 = (ax + bx + cx) / 3.f, mz3 = (az + bz + cz) / 3.f;
                    if (nx * (px - mx3) + nz * (pz - mz3) < 0.f) { nx = -nx; nz = -nz; }
                    float nlen = std::sqrt(nx * nx + nz * nz);
                    if (nlen > 1e-4f) { px += nx / nlen * R; pz += nz / nlen * R; }
                }
            }
        }
    }
    float head = (float)m.y;
    float feet = head + m.h;
    bool any = false;
    float surf = 0.f;
    float gap = 0.f;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        if (s == slot) continue;
        const Entity& o = g.entities[s];
        if (!o.active || !o.hasBody || o.bodyType != 0) continue;
        if (o.kind != "model" || o.hitbox <= 0) continue;
        const std::vector<float>& skin = hitbox_cache(o).skin;
        if (skin.size() < 9) continue;
        int od = solid_depth(o);
        float ocx = o.x + o.w * 0.5f;
        float ocy = o.y + o.h * 0.5f;
        float ocz = o.z + od * 0.5f;
        for (size_t i = 0; i + 8 < skin.size(); i += 9) {
            float ax = ocx + skin[i],     ay = ocy + skin[i + 1], az = ocz + skin[i + 2];
            float bx = ocx + skin[i + 3], by = ocy + skin[i + 4], bz = ocz + skin[i + 5];
            float cx = ocx + skin[i + 6], cy = ocy + skin[i + 7], cz = ocz + skin[i + 8];
            float minx = std::min(ax, std::min(bx, cx)) - 4.f;
            float maxx = std::max(ax, std::max(bx, cx)) + 4.f;
            float minz = std::min(az, std::min(bz, cz)) - 4.f;
            float maxz = std::max(az, std::max(bz, cz)) + 4.f;
            if (px < minx || px > maxx || pz < minz || pz > maxz) continue;
            float ex = (by - ay) * (cz - az) - (bz - az) * (cy - ay);
            float ey = (bz - az) * (cx - ax) - (bx - ax) * (cz - az);
            float ez = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            float el2 = ex * ex + ey * ey + ez * ez;
            if (el2 < 1.f) continue;
            if (!(std::fabs(ey) >= std::fabs(ex) && std::fabs(ey) >= std::fabs(ez) && ey < 0.f)) continue;
            if (!in_tri(px, pz, ax, az, bx, bz, cx, cz)) continue;
            float yhit = ay - (ex * (px - ax) + ez * (pz - az)) / ey;
            bool straddle = head < yhit && feet > yhit;
            bool close = yhit <= feet + 8.f && yhit >= feet - 8.f;
            if (!straddle && !close) continue;
            float g = std::fabs(yhit - feet);
            if (!any || g < gap) { any = true; gap = g; surf = yhit; }
        }
    }
    int nx = (int)std::lround(px - m.w * 0.5f);
    int nz = (int)std::lround(pz - depth * 0.5f);
    int ny = any ? (int)std::lround(surf - m.h) : m.y;
    if (nx == m.x && ny == m.y && nz == m.z) return;
    g_physWriteback = 1;
    engine_entity_set_x(slot, nx);
    engine_entity_set_y(slot, ny);
    g_physWriteback = 0;
    engine_entity_set_z(slot, nz);
    if (!m.body) return;
    float ppm = g.pixelsPerMeter;
    if (ppm < 1.f) ppm = 50.f;
    b2Vec2 v = m.body->GetLinearVelocity();
    if (any && v.y > 0.f) v.y = 0.f;
    m.body->SetLinearVelocity(v);
    m.body->SetTransform(b2Vec2((m.x + m.w * 0.5f) / ppm, (m.y + m.h * 0.5f) / ppm), m.body->GetAngle());
}

static int g_gamePaused = 0;
static int g_leavePlay = 0;

extern "C" void engine_set_paused(int on) {
    g_gamePaused = on ? 1 : 0;
    if (g_gamePaused) engine_mouse_lock(0);
}
extern "C" int engine_paused() { return g_gamePaused; }
extern "C" void engine_request_leave_play() { g_leavePlay = 1; }
extern "C" int engine_take_leave_play() {
    int v = g_leavePlay;
    g_leavePlay = 0;
    return v;
}

extern "C" void engine_physics_step(double dt) {
    if (!g.physWorld || g_gamePaused) return;
    float step = (float)dt;
    if (step <= 0.0f) step = 1.0f / 60.0f;
    if (step > 0.05f) step = 0.05f;   /* clamp so a stall can't explode the sim */
    g.physWorld->Step(step, 8, 3);

    float ppm = g.pixelsPerMeter;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        Entity& e = g.entities[s];
        if (!e.active || !e.hasBody || !e.body) continue;
        if (e.bodyType == 0) continue;  /* static bodies never move */
        b2Vec2 p = e.body->GetPosition();
        int w = e.w < 4 ? 4 : e.w;
        int h = e.h < 4 ? 4 : e.h;
        int nx = (int)lroundf(p.x * ppm - w / 2.0f);
        int ny = (int)lroundf(p.y * ppm - h / 2.0f);
        /* use set_x/set_y so any child (e.g. a follow camera) tracks the body */
        g_physWriteback = 1;
        engine_entity_set_x(s, nx);
        engine_entity_set_y(s, ny);
        g_physWriteback = 0;
        if (!e.fixedRotation) {
            int deg = (int)lroundf(e.body->GetAngle() * kRad2Deg);
            deg %= 360;
            if (deg < 0) deg += 360;
            e.rotDeg = deg;
        }
        mesh_hold(s);
    }
}

extern "C" void engine_physics_end() {
    if (g.physWorld) {
        delete g.physWorld;   /* frees all bodies/fixtures */
        g.physWorld = nullptr;
    }
    for (Entity& e : g.entities) e.body = nullptr;
}

/* ---- per-entity rigidbody component accessors ---- */
extern "C" int  engine_entity_has_body(int s) { return active_slot(s) && g.entities[s].hasBody ? 1 : 0; }
extern "C" void engine_entity_set_body(int s, int on) { if (active_slot(s)) g.entities[s].hasBody = on != 0; }
extern "C" int  engine_entity_body_type(int s) { return active_slot(s) ? g.entities[s].bodyType : 0; }
extern "C" void engine_entity_set_body_type(int s, int t) {
    if (!active_slot(s)) return;
    if (t < 0) t = 0; if (t > 2) t = 2;
    g.entities[s].bodyType = t;
    if (g.entities[s].body) g.entities[s].body->SetType(box2d_type(t));
}
extern "C" float engine_entity_density(int s) { return active_slot(s) ? g.entities[s].density : 0.0f; }
extern "C" void  engine_entity_set_density(int s, float v) {
    if (!active_slot(s)) return;
    g.entities[s].density = v;
    if (g.entities[s].body) {
        for (b2Fixture* f = g.entities[s].body->GetFixtureList(); f; f = f->GetNext()) f->SetDensity(v);
        g.entities[s].body->ResetMassData();
    }
}
extern "C" float engine_entity_friction(int s) { return active_slot(s) ? g.entities[s].friction : 0.0f; }
extern "C" void  engine_entity_set_friction(int s, float v) {
    if (!active_slot(s)) return;
    g.entities[s].friction = v;
    if (g.entities[s].body)
        for (b2Fixture* f = g.entities[s].body->GetFixtureList(); f; f = f->GetNext()) f->SetFriction(v);
}
extern "C" float engine_entity_restitution(int s) { return active_slot(s) ? g.entities[s].restitution : 0.0f; }
extern "C" void  engine_entity_set_restitution(int s, float v) {
    if (!active_slot(s)) return;
    g.entities[s].restitution = v;
    if (g.entities[s].body)
        for (b2Fixture* f = g.entities[s].body->GetFixtureList(); f; f = f->GetNext()) f->SetRestitution(v);
}
extern "C" float engine_entity_gravity_scale(int s) { return active_slot(s) ? g.entities[s].gravityScale : 0.0f; }
extern "C" void  engine_entity_set_gravity_scale(int s, float v) {
    if (!active_slot(s)) return;
    g.entities[s].gravityScale = v;
    if (g.entities[s].body) g.entities[s].body->SetGravityScale(v);
}
extern "C" int  engine_entity_fixed_rotation(int s) { return active_slot(s) && g.entities[s].fixedRotation ? 1 : 0; }
extern "C" int engine_entity_ui_value(int s) {
    return active_slot(s) ? g.entities[s].uiValue : 0;
}
extern "C" void engine_entity_set_ui_value(int s, int v) {
    if (!active_slot(s)) return;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g.entities[s].uiValue = v;
}
extern "C" const char* engine_ascii_char(int code) {
    static char buf[2];
    if (code < 32 || code > 126) { buf[0] = 0; return buf; }
    buf[0] = (char)code;
    buf[1] = 0;
    return buf;
}
extern "C" int engine_entity_hitbox(int s) {
    return active_slot(s) ? g.entities[s].hitbox : 0;
}
extern "C" void engine_entity_set_hitbox(int s, int level) {
    if (!active_slot(s)) return;
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    Entity& e = g.entities[s];
    if (e.hitbox == level) return;
    e.hitbox = level;
    physics_rebuild_body(e);
}
extern "C" int engine_entity_hitbox_count(int s) {
    if (!active_slot(s)) return 0;
    return (int)hitbox_pieces(g.entities[s]).size();
}
extern "C" float engine_entity_hitbox_cx(int s, int i) {
    if (!active_slot(s)) return 0;
    const auto& p = hitbox_pieces(g.entities[s]);
    if (i < 0 || i >= (int)p.size()) return 0;
    return p[i].cx;
}
extern "C" float engine_entity_hitbox_cy(int s, int i) {
    if (!active_slot(s)) return 0;
    const auto& p = hitbox_pieces(g.entities[s]);
    if (i < 0 || i >= (int)p.size()) return 0;
    return p[i].cy;
}
extern "C" float engine_entity_hitbox_hw(int s, int i) {
    if (!active_slot(s)) return 0;
    const auto& p = hitbox_pieces(g.entities[s]);
    if (i < 0 || i >= (int)p.size()) return 0;
    return p[i].hw;
}
extern "C" float engine_entity_hitbox_hh(int s, int i) {
    if (!active_slot(s)) return 0;
    const auto& p = hitbox_pieces(g.entities[s]);
    if (i < 0 || i >= (int)p.size()) return 0;
    return p[i].hh;
}
extern "C" void engine_entity_set_fixed_rotation(int s, int on) {
    if (!active_slot(s)) return;
    g.entities[s].fixedRotation = on != 0;
    if (g.entities[s].body) g.entities[s].body->SetFixedRotation(on != 0);
}

/* ---- script-facing body controls (only meaningful while sim is active) ---- */
extern "C" void engine_entity_apply_force(int s, float fx, float fy) {
    if (active_slot(s) && g.entities[s].body)
        g.entities[s].body->ApplyForceToCenter(b2Vec2(fx / g.pixelsPerMeter, fy / g.pixelsPerMeter), true);
}
extern "C" void engine_entity_apply_impulse(int s, float ix, float iy) {
    if (active_slot(s) && g.entities[s].body)
        g.entities[s].body->ApplyLinearImpulseToCenter(b2Vec2(ix / g.pixelsPerMeter, iy / g.pixelsPerMeter), true);
}
extern "C" void engine_entity_set_velocity(int s, float vx, float vy) {
    if (active_slot(s) && g.entities[s].body)
        g.entities[s].body->SetLinearVelocity(b2Vec2(vx / g.pixelsPerMeter, vy / g.pixelsPerMeter));
}
extern "C" float engine_entity_velocity_x(int s) {
    if (active_slot(s) && g.entities[s].body) return g.entities[s].body->GetLinearVelocity().x * g.pixelsPerMeter;
    return 0.0f;
}
extern "C" float engine_entity_velocity_y(int s) {
    if (active_slot(s) && g.entities[s].body) return g.entities[s].body->GetLinearVelocity().y * g.pixelsPerMeter;
    return 0.0f;
}

extern "C" int engine_scene_save(const char* path) {
    if (!path) return 0;
    std::ofstream f(path, std::ios::binary);
    if (!f) return 0;
    f << "EYESCIRE_SCENE 8\n";
    f << "NEXT_ID " << g.nextEntityId << "\n";
    f << "GRAVITY " << g.gravityX << " " << g.gravityY << "\n";
    f << "PPM " << g.pixelsPerMeter << "\n";
    f << "CLEAR " << g.clearR << " " << g.clearG << " " << g.clearB << "\n";
    f << "FOV " << g.sceneFov << "\n";
    if (g.skyMode == 1) f << "SKY sunset\n";
    else if (g.skyMode == 2) f << "SKY night\n";
    else if (g.skyMode == 4) f << "SKY solid\n";
    else if (g.skyMode == 3) {
        f << "SKY custom";
        for (int i = 0; i < 6; ++i) f << (i ? "|" : " ") << g.skyFace[i];
        f << "\n";
    } else f << "SKY day\n";
    if (g.render3d) f << "RENDER 3D\n";
    for (const Entity& e : g.entities) {
        if (!e.active) continue;
        int parentId = 0;
        if (e.parentSlot >= 0 && active_slot(e.parentSlot))
            parentId = g.entities[e.parentSlot].id;
        f << "ENTITY " << e.id << "|" << e.name << "|" << e.kind << "|"
          << e.x << "|" << e.y << "|" << e.w << "|" << e.h << "|"
          << e.r << "|" << e.g << "|" << e.b << "|"
          << e.sprite << "|" << e.text << "|" << e.script << "|"
          << parentId << "|" << e.rotDeg << "|"
          << (e.isCamera ? 1 : 0) << "|"
          << (e.hasBody ? 1 : 0) << "|" << e.bodyType << "|"
          << e.density << "|" << e.friction << "|" << e.restitution << "|"
          << e.gravityScale << "|" << (e.fixedRotation ? 1 : 0) << "|"
          << e.z << "|" << e.depth << "|" << e.model << "|"
          << e.lightIntensity << "|" << e.lightRange << "|"
          << e.lightAngle << "|" << e.lightTilt << "|"
          << (e.visible ? 1 : 0) << "|"
          << (e.hierExpanded ? 1 : 0) << "|"
          << e.btnHover.r << "|" << e.btnHover.g << "|" << e.btnHover.b << "|" << e.btnHover.custom << "|"
          << e.btnPress.r << "|" << e.btnPress.g << "|" << e.btnPress.b << "|" << e.btnPress.custom << "|"
          << e.btnToggle << "|" << e.btnToggled << "|"
          << e.btnOn.r << "|" << e.btnOn.g << "|" << e.btnOn.b << "|" << e.btnOn.custom << "|"
          << e.btnOnHover.r << "|" << e.btnOnHover.g << "|" << e.btnOnHover.b << "|" << e.btnOnHover.custom << "|"
          << e.a << "|" << e.btnScript << "|" << e.btnFn << "|" << e.hitbox << "|"
          << e.uiValue << "|" << e.rotX << "|" << e.rotY << "|"
          << e.sound << "|" << (e.soundLoop ? 1 : 0) << "|" << e.soundRange << "|"
          << e.emitRate << "|" << e.emitLife << "|" << e.emitSpeed << "|"
          << e.emitSpread << "|" << e.emitSize << "|" << (e.emitHD ? 1 : 0) << "\n";
    }
    return 1;
}

extern "C" int engine_scene_load(const char* path) {
    if (!path) return 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    engine_tilemap_destroy();
    engine_entity_clear_all();
    /* reset scene gravity to the default; a GRAVITY line overrides it */
    g.gravityX = 0.0f;
    g.gravityY = 980.0f;
    g.pixelsPerMeter = 50.0f;
    g.clearR = 28; g.clearG = 32; g.clearB = 40;
    g.sceneFov = 60.0f;
    g.skyMode = 4;
    for (int i = 0; i < 6; ++i) g.skyFace[i].clear();
    g.render3d = false;
    std::string line;
    int header_ok = 0;
    struct PendingParent { int slot; int parentId; };
    std::vector<PendingParent> pending;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("EYESCIRE_SCENE", 0) == 0) { header_ok = 1; continue; }
        if (line.rfind("NEXT_ID ", 0) == 0) {
            try { g.nextEntityId = std::stoi(line.substr(8)); } catch (...) {}
            continue;
        }
        if (line.rfind("GRAVITY ", 0) == 0) {
            std::istringstream gs(line.substr(8));
            gs >> g.gravityX >> g.gravityY;
            continue;
        }
        if (line.rfind("PPM ", 0) == 0) {
            try { g.pixelsPerMeter = std::stof(line.substr(4)); } catch (...) {}
            if (g.pixelsPerMeter < 1.0f) g.pixelsPerMeter = 50.0f;
            if (g.pixelsPerMeter > 400.0f) g.pixelsPerMeter = 400.0f;
            continue;
        }
        if (line.rfind("CLEAR ", 0) == 0) {
            std::istringstream cs(line.substr(6));
            int cr = 28, cg = 32, cb = 40;
            cs >> cr >> cg >> cb;
            engine_scene_set_clear(cr, cg, cb);
            continue;
        }
        if (line.rfind("FOV ", 0) == 0) {
            float deg = 60.0f;
            try { deg = std::stof(line.substr(4)); } catch (...) {}
            engine_scene_set_fov(deg);
            continue;
        }
        if (line.rfind("RENDER ", 0) == 0) {
            g.render3d = line.substr(7) == "3D";
            continue;
        }
        if (line.rfind("SKY ", 0) == 0) {
            std::string rest = line.substr(4);
            if (rest.rfind("custom", 0) == 0) {
                g.skyMode = 3;
                std::string faces = rest.size() > 7 ? rest.substr(7) : "";
                auto parts = split_pipe(faces);
                for (int i = 0; i < 6 && i < (int)parts.size(); ++i) g.skyFace[i] = parts[i];
            } else if (rest == "sunset") g.skyMode = 1;
            else if (rest == "night") g.skyMode = 2;
            else if (rest == "solid") g.skyMode = 4;
            else g.skyMode = 0;
            continue;
        }
        if (line.rfind("ENTITY ", 0) == 0) {
            auto parts = split_pipe(line.substr(7));
            if (parts.size() < 10) continue;
            for (int si = 0; si < (int)g.entities.size(); ++si) {
                Entity& e = g.entities[si];
                if (e.active) continue;
                e = Entity{};
                e.active = true;
                try { e.id = std::stoi(parts[0]); } catch (...) { e.id = 0; }
                e.name = parts[1];
                e.kind = parts[2];
                try { e.x = std::stoi(parts[3]); } catch (...) {}
                try { e.y = std::stoi(parts[4]); } catch (...) {}
                try { e.w = std::stoi(parts[5]); } catch (...) {}
                try { e.h = std::stoi(parts[6]); } catch (...) {}
                try { e.r = std::stoi(parts[7]); } catch (...) {}
                try { e.g = std::stoi(parts[8]); } catch (...) {}
                try { e.b = std::stoi(parts[9]); } catch (...) {}
                if (parts.size() >= 11) e.sprite = parts[10];
                if (parts.size() >= 12) e.text   = parts[11];
                if (parts.size() >= 13) e.script = parts[12];
                if (!e.sprite.empty()) {
                    std::string p = asset_resolve(e.sprite);
                    e.spriteId = engine_image_load(p.c_str());
                }
                if (parts.size() >= 14) {
                    int parentId = 0;
                    try { parentId = std::stoi(parts[13]); } catch (...) {}
                    if (parentId > 0) pending.push_back({ si, parentId });
                }
                if (parts.size() >= 15) {
                    try { e.rotDeg = std::stoi(parts[14]); } catch (...) {}
                }
                if (parts.size() >= 16) {
                    try { e.isCamera = std::stoi(parts[15]) != 0; } catch (...) {}
                } else if (e.kind == "camera") {
                    e.isCamera = true;
                }
                /* scene v6: physics / rigidbody fields */
                if (parts.size() >= 17) { try { e.hasBody = std::stoi(parts[16]) != 0; } catch (...) {} }
                if (parts.size() >= 18) { try { e.bodyType = std::stoi(parts[17]); } catch (...) {} }
                if (parts.size() >= 19) { try { e.density = std::stof(parts[18]); } catch (...) {} }
                if (parts.size() >= 20) { try { e.friction = std::stof(parts[19]); } catch (...) {} }
                if (parts.size() >= 21) { try { e.restitution = std::stof(parts[20]); } catch (...) {} }
                if (parts.size() >= 22) { try { e.gravityScale = std::stof(parts[21]); } catch (...) {} }
                if (parts.size() >= 23) { try { e.fixedRotation = std::stoi(parts[22]) != 0; } catch (...) {} }
                /* scene v7: 3D */
                if (parts.size() >= 24) { try { e.z = std::stoi(parts[23]); } catch (...) {} }
                if (parts.size() >= 25) { try { e.depth = std::stoi(parts[24]); } catch (...) {} }
                if (parts.size() >= 26) e.model = parts[25];
                /* scene v8: lights */
                if (parts.size() >= 27) { try { e.lightIntensity = std::stoi(parts[26]); } catch (...) {} }
                if (parts.size() >= 28) { try { e.lightRange = std::stoi(parts[27]); } catch (...) {} }
                if (parts.size() >= 29) { try { e.lightAngle = std::stoi(parts[28]); } catch (...) {} }
                if (parts.size() >= 30) { try { e.lightTilt = std::stoi(parts[29]); } catch (...) {} }
                if (parts.size() >= 31) { try { e.visible = std::stoi(parts[30]) != 0; } catch (...) {} }
                if (parts.size() >= 32) { try { e.hierExpanded = std::stoi(parts[31]) != 0; } catch (...) {} }
                auto take = [&](int idx, int& dst) {
                    if ((int)parts.size() <= idx) return;
                    try { dst = std::stoi(parts[idx]); } catch (...) {}
                };
                take(32, e.btnHover.r); take(33, e.btnHover.g); take(34, e.btnHover.b); take(35, e.btnHover.custom);
                take(36, e.btnPress.r); take(37, e.btnPress.g); take(38, e.btnPress.b); take(39, e.btnPress.custom);
                take(40, e.btnToggle); take(41, e.btnToggled);
                take(42, e.btnOn.r); take(43, e.btnOn.g); take(44, e.btnOn.b); take(45, e.btnOn.custom);
                take(46, e.btnOnHover.r); take(47, e.btnOnHover.g); take(48, e.btnOnHover.b); take(49, e.btnOnHover.custom);
                take(50, e.a);
                if (parts.size() >= 52) e.btnScript = parts[51];
                if (parts.size() >= 53) e.btnFn = parts[52];
                if (parts.size() >= 54) {
                    try { e.hitbox = std::stoi(parts[53]); } catch (...) {}
                    if (e.hitbox < 0) e.hitbox = 0;
                    if (e.hitbox > 5) e.hitbox = 5;
                }
                if (parts.size() >= 55) {
                    try { e.uiValue = std::stoi(parts[54]); } catch (...) {}
                    if (e.uiValue < 0) e.uiValue = 0;
                    if (e.uiValue > 100) e.uiValue = 100;
                }
                if (parts.size() >= 56) { try { e.rotX = wrap360(std::stoi(parts[55])); } catch (...) {} }
                if (parts.size() >= 57) { try { e.rotY = wrap360(std::stoi(parts[56])); } catch (...) {} }
                if (parts.size() >= 58) e.sound = parts[57];
                if (parts.size() >= 59) { try { e.soundLoop = std::stoi(parts[58]) != 0; } catch (...) {} }
                if (parts.size() >= 60) {
                    try { e.soundRange = std::stoi(parts[59]); } catch (...) {}
                    if (e.soundRange < 50) e.soundRange = 50;
                    if (e.soundRange > 20000) e.soundRange = 20000;
                }
                if (parts.size() >= 61) { try { e.emitRate = std::stoi(parts[60]); } catch (...) {} }
                if (parts.size() >= 62) { try { e.emitLife = std::stoi(parts[61]); } catch (...) {} }
                if (parts.size() >= 63) { try { e.emitSpeed = std::stoi(parts[62]); } catch (...) {} }
                if (parts.size() >= 64) { try { e.emitSpread = std::stoi(parts[63]); } catch (...) {} }
                if (parts.size() >= 65) { try { e.emitSize = std::stoi(parts[64]); } catch (...) {} }
                if (parts.size() >= 66) { try { e.emitHD = std::stoi(parts[65]) != 0; } catch (...) {} }
                if (e.emitRate < 0) e.emitRate = 0;
                if (e.emitRate > 200) e.emitRate = 200;
                if (e.emitLife < 50) e.emitLife = 50;
                if (e.emitLife > 10000) e.emitLife = 10000;
                if (e.emitSpeed < 0) e.emitSpeed = 0;
                if (e.emitSpeed > 2000) e.emitSpeed = 2000;
                if (e.emitSpread < 0) e.emitSpread = 0;
                if (e.emitSpread > 180) e.emitSpread = 180;
                if (e.emitSize < 2) e.emitSize = 2;
                if (e.emitSize > 256) e.emitSize = 256;
                e.emitHD = e.emitHD ? 1 : 0;
                e.btnHover.r = clamp_ch(e.btnHover.r); e.btnHover.g = clamp_ch(e.btnHover.g); e.btnHover.b = clamp_ch(e.btnHover.b);
                e.btnPress.r = clamp_ch(e.btnPress.r); e.btnPress.g = clamp_ch(e.btnPress.g); e.btnPress.b = clamp_ch(e.btnPress.b);
                e.btnOn.r = clamp_ch(e.btnOn.r); e.btnOn.g = clamp_ch(e.btnOn.g); e.btnOn.b = clamp_ch(e.btnOn.b);
                e.btnOnHover.r = clamp_ch(e.btnOnHover.r); e.btnOnHover.g = clamp_ch(e.btnOnHover.g); e.btnOnHover.b = clamp_ch(e.btnOnHover.b);
                e.a = clamp_ch(e.a);
                e.btnHover.custom = e.btnHover.custom ? 1 : 0;
                e.btnPress.custom = e.btnPress.custom ? 1 : 0;
                e.btnOn.custom = e.btnOn.custom ? 1 : 0;
                e.btnOnHover.custom = e.btnOnHover.custom ? 1 : 0;
                e.btnToggle = e.btnToggle ? 1 : 0;
                e.btnToggled = e.btnToggled ? 1 : 0;
                if (e.id >= g.nextEntityId) g.nextEntityId = e.id + 1;
                break;
            }
        }
    }
    for (auto& pp : pending) {
        int ps = engine_entity_find_by_id(pp.parentId);
        if (ps >= 0) engine_entity_set_parent(pp.slot, ps);
    }
    part_reset();
    sfx_on_scene_loaded();
    g_gamePaused = 0;
    return header_ok;
}

/* ============================================================ */
/*  File system helpers                                         */
/* ============================================================ */

extern "C" int engine_fs_make_dir(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    fs::create_directories(path, ec);
    return ec ? 0 : 1;
}

extern "C" int engine_fs_exists(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    return fs::exists(path, ec) ? 1 : 0;
}

extern "C" int engine_fs_is_dir(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    return fs::is_directory(path, ec) ? 1 : 0;
}

extern "C" long long engine_fs_mtime(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return (long long)t.time_since_epoch().count();
}

extern "C" const char* engine_fs_list(const char* dir, int i) {
    if (!dir) return nullptr;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return nullptr;
    int idx = 0;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) return nullptr;
        if (idx == i) {
            g_fsListEntry = entry.path().filename().string();
            return g_fsListEntry.c_str();
        }
        ++idx;
    }
    return nullptr;
}

extern "C" int engine_fs_copy_file(const char* src, const char* dst) {
    if (!src || !dst) return 0;
    std::error_code ec;
    fs::path d = dst;
    if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return ec ? 0 : 1;
}

extern "C" int engine_fs_copy_tree(const char* src, const char* dst) {
    if (!src || !dst) return 0;
    std::error_code ec;
    if (!fs::exists(src, ec)) return 0;
    fs::create_directories(dst, ec);
    fs::copy(src, dst,
             fs::copy_options::recursive |
             fs::copy_options::overwrite_existing,
             ec);
    return ec ? 0 : 1;
}

extern "C" int engine_fs_remove_tree(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    fs::remove_all(path, ec);
    return ec ? 0 : 1;
}

extern "C" int engine_fs_remove_file(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    fs::remove(path, ec);
    return ec ? 0 : 1;
}

static std::string settings_file_path() {
    if (g.projLoaded && !g.projRoot.empty())
        return (fs::path(g.projRoot) / "settings.cfg").string();
    return std::string(engine_exe_dir()) + "\\settings.cfg";
}

static int parse_setting_int(const std::string& line, const char* key, int def) {
    const size_t klen = std::strlen(key);
    if (line.size() <= klen || line.compare(0, klen, key) != 0) return def;
    try { return std::stoi(line.substr(klen)); } catch (...) { return def; }
}

extern "C" int engine_settings_load() {
    std::string path = settings_file_path();
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        g.settingsRes        = parse_setting_int(line, "res=", g.settingsRes);
        g.settingsFullscreen = parse_setting_int(line, "fullscreen=", g.settingsFullscreen);
        g.settingsMusic      = parse_setting_int(line, "music=", g.settingsMusic);
        g.settingsSfx        = parse_setting_int(line, "sfx=", g.settingsSfx);
        g.settingsParallax   = parse_setting_int(line, "parallax=", g.settingsParallax);
        g.settingsAutosave   = parse_setting_int(line, "autosave=", g.settingsAutosave);
        g.settingsUiScale    = parse_setting_int(line, "uiscale=", g.settingsUiScale);
    }
    return 1;
}

extern "C" void engine_settings_save(int res, int fullscreen, int music, int sfx,
                                     int parallax, int autosave, int uiscale) {
    g.settingsRes = res;
    g.settingsFullscreen = fullscreen;
    g.settingsMusic = music;
    g.settingsSfx = sfx;
    g.settingsParallax = parallax;
    g.settingsAutosave = autosave;
    g.settingsUiScale = uiscale;

    std::string path = settings_file_path();
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    f << "res=" << res << "\n"
      << "fullscreen=" << fullscreen << "\n"
      << "music=" << music << "\n"
      << "sfx=" << sfx << "\n"
      << "parallax=" << parallax << "\n"
      << "autosave=" << autosave << "\n"
      << "uiscale=" << uiscale << "\n";
}

extern "C" int engine_settings_res() { return g.settingsRes; }
extern "C" int engine_settings_fullscreen() { return g.settingsFullscreen; }
extern "C" int engine_settings_music() { return g.settingsMusic; }
extern "C" int engine_settings_sfx() { return g.settingsSfx; }
extern "C" int engine_settings_parallax() { return g.settingsParallax; }
extern "C" int engine_settings_autosave() { return g.settingsAutosave; }
extern "C" int engine_settings_uiscale() { return g.settingsUiScale; }

static std::string worlds_root_dir() {
    if (g.projLoaded && !g.projRoot.empty())
        return (fs::path(g.projRoot) / "worlds").string();
    return std::string(engine_exe_dir()) + "\\worlds";
}

static std::string default_world_folder_name(int n) {
    return "World " + std::to_string(n);
}

extern "C" void engine_worlds_refresh() {
    g.worldNames.clear();
    std::string root = worlds_root_dir();
    std::error_code ec;
    fs::create_directories(root, ec);
    if (!fs::is_directory(root, ec)) return;
    for (auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        g.worldNames.push_back(entry.path().filename().string());
    }
    std::sort(g.worldNames.begin(), g.worldNames.end());
}

extern "C" int engine_worlds_count() {
    return (int)g.worldNames.size();
}

extern "C" const char* engine_worlds_name(int i) {
    if (i < 0 || i >= (int)g.worldNames.size()) return "";
    g.worldScratch = g.worldNames[(size_t)i];
    return g.worldScratch.c_str();
}

extern "C" const char* engine_worlds_path(int i) {
    if (i < 0 || i >= (int)g.worldNames.size()) return "";
    g.worldScratch = (fs::path(worlds_root_dir()) / g.worldNames[(size_t)i]).string();
    return g.worldScratch.c_str();
}

static char g_editName[48] = "";
static char g_editSeed[48] = "";
static int  g_editFocus = 0;
static int  g_editActive = 0;

static std::string sanitize_world_folder_name(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            continue;
        if (c >= 32 && c < 127) out.push_back(c);
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "World";
    if (out.size() > 40) out.resize(40);
    return out;
}

static bool write_world_cfg(const fs::path& dir, const std::string& display_name,
                            int size_idx, int diff_idx, int evil_idx, const char* seed) {
    fs::path cfg = dir / "world.cfg";
    FILE* f = nullptr;
    if (fopen_s(&f, cfg.string().c_str(), "wb") != 0 || !f) return false;
    fprintf(f, "name=%s\nsize=%d\ndifficulty=%d\ntheme=%d\n",
            display_name.c_str(), size_idx, diff_idx, evil_idx);
    if (seed && seed[0]) fprintf(f, "seed=%s\n", seed);
    else fprintf(f, "seed=random\n");
    fclose(f);
    return true;
}

static int worlds_index_of_name(const std::string& folder) {
    for (int i = 0; i < (int)g.worldNames.size(); ++i) {
        if (g.worldNames[(size_t)i] == folder) return i;
    }
    return -1;
}

extern "C" int engine_worlds_create_named(const char* name, int size_idx, int diff_idx,
                                          int evil_idx, const char* seed) {
    if (size_idx < 0) size_idx = 0;
    if (size_idx > 2) size_idx = 2;
    if (diff_idx < 0) diff_idx = 0;
    if (diff_idx > 3) diff_idx = 3;
    if (evil_idx < 0) evil_idx = 0;
    if (evil_idx > 3) evil_idx = 3;

    std::string base = sanitize_world_folder_name(name ? name : "");
    std::string root = worlds_root_dir();
    std::error_code ec;
    fs::create_directories(root, ec);

    std::string folder = base;
    for (int attempt = 0; attempt < 99; ++attempt) {
        fs::path path = fs::path(root) / folder;
        if (!fs::exists(path, ec)) {
            fs::create_directories(path, ec);
            if (ec) return -1;
            write_world_cfg(path, base, size_idx, diff_idx, evil_idx, seed);
            engine_worlds_refresh();
            int idx = worlds_index_of_name(folder);
            if (idx >= 0) return idx;
            return (int)g.worldNames.size() - 1;
        }
        folder = base + " " + std::to_string(attempt + 2);
    }
    return -1;
}

extern "C" int engine_worlds_create() {
    for (int n = 1; n <= 99; ++n) {
        std::string name = default_world_folder_name(n);
        fs::path path = fs::path(worlds_root_dir()) / name;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return engine_worlds_create_named(name.c_str(), 1, 0, 0, nullptr);
        }
    }
    return -1;
}

extern "C" int engine_worlds_create_from_edit(int size_idx, int diff_idx, int evil_idx) {
    return engine_worlds_create_named(g_editName, size_idx, diff_idx, evil_idx, g_editSeed);
}

extern "C" void engine_edit_clear() { g_editName[0] = '\0'; }

extern "C" void engine_edit_set(const char* s) {
    if (!s) { g_editName[0] = '\0'; return; }
    strncpy_s(g_editName, s, 47);
    g_editName[47] = '\0';
}

extern "C" const char* engine_edit_text() { return g_editName; }

extern "C" void engine_edit_clear_seed() { g_editSeed[0] = '\0'; }

extern "C" const char* engine_edit_seed_text() { return g_editSeed; }

extern "C" void engine_edit_focus(int slot) {
    if (slot == 1) g_editFocus = 1;
    else g_editFocus = 0;
}

extern "C" void engine_edit_active(int on) { g_editActive = on ? 1 : 0; }

extern "C" int engine_edit_text_width() { return engine_text_width(g_editName); }

extern "C" void engine_edit_poll() {
    if (!g_editActive) return;
    char* buf = g_editFocus ? g_editSeed : g_editName;
    int c = 0;
    while ((c = engine_typed_pop()) != 0) {
        size_t len = strlen(buf);
        if (c == 8 || c == 127) {
            if (len > 0) buf[len - 1] = '\0';
            continue;
        }
        if (c == 13 || c == 27) continue;
        if (c >= 32 && c < 127 && len < 47) {
            buf[len] = (char)c;
            buf[len + 1] = '\0';
        }
    }
}

extern "C" int engine_worlds_delete(int i) {
    if (i < 0 || i >= (int)g.worldNames.size()) return 0;
    fs::path path = fs::path(worlds_root_dir()) / g.worldNames[(size_t)i];
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) return 0;
    engine_worlds_refresh();
    return 1;
}

static std::string players_root_dir() {
    if (g.projLoaded && !g.projRoot.empty())
        return (fs::path(g.projRoot) / "players").string();
    return std::string(engine_exe_dir()) + "\\players";
}

static std::string default_player_folder_name(int n) {
    return "Player " + std::to_string(n);
}

static std::string sanitize_player_folder_name(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            continue;
        if (c >= 32 && c < 127) out.push_back(c);
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "Player";
    if (out.size() > 40) out.resize(40);
    return out;
}

static int g_editHair = 0;
static int g_editShirt = 0;
static int g_editPants = 0;
static int g_editHat = 0;
static int g_editAcc = 3;
static int g_editSkinRgb = 0xFFA276;
static int g_editHairRgb = 0x9C4126;
static int g_editShirtRgb = 0x37AB4C;
static int g_editPantsRgb = 0xFF8C23;
static int g_editHatRgb = 0xDCB430;
static int g_editAccRgb = 0xA86C34;
static int g_editUndershirtRgb = 0xA0B4D7;
static int g_editShoesRgb = 0x322A28;

static int clamp_look(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static const int kOldSkinRgb[] = { 0xFFA276, 0xD89458, 0x985C38, 0xFFC494 };
static const int kOldHairRgb[] = { 0x9C4126, 0xE4C448, 0x2A201C, 0xE4E4EC, 0xC44030, 0x20A4B0 };
static const int kOldShirtRgb[] = { 0x37AB4C, 0x3870C4, 0xC43838, 0xE8E8EE, 0x383444, 0xD4A42C };
static const int kOldPantsRgb[] = { 0xFF8C23, 0x3054A0, 0x28282E, 0x307C38 };

static EngineState::PlayerLook read_player_look(const fs::path& dir) {
    EngineState::PlayerLook look{};
    fs::path cfg = dir / "player.cfg";
    FILE* f = nullptr;
    if (fopen_s(&f, cfg.string().c_str(), "rb") != 0 || !f) return look;
    int old_skin = -1;
    int has_skin_rgb = 0;
    int has_hair_rgb = 0;
    int has_shirt_rgb = 0;
    int has_pants_rgb = 0;
    int has_under_rgb = 0;
    int has_shoes_rgb = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (sscanf_s(line, "skin_rgb=%d", &v) == 1) { look.skin_rgb = v; has_skin_rgb = 1; }
        else if (sscanf_s(line, "hair_rgb=%d", &v) == 1) { look.hair_rgb = v; has_hair_rgb = 1; }
        else if (sscanf_s(line, "shirt_rgb=%d", &v) == 1) { look.shirt_rgb = v; has_shirt_rgb = 1; }
        else if (sscanf_s(line, "pants_rgb=%d", &v) == 1) { look.pants_rgb = v; has_pants_rgb = 1; }
        else if (sscanf_s(line, "undershirt_rgb=%d", &v) == 1) { look.undershirt_rgb = v; has_under_rgb = 1; }
        else if (sscanf_s(line, "shoes_rgb=%d", &v) == 1) { look.shoes_rgb = v; has_shoes_rgb = 1; }
        else if (sscanf_s(line, "hat_rgb=%d", &v) == 1) look.hat_rgb = v;
        else if (sscanf_s(line, "acc_rgb=%d", &v) == 1) look.acc_rgb = v;
        else if (sscanf_s(line, "skin=%d", &v) == 1) old_skin = v;
        else if (sscanf_s(line, "hair=%d", &v) == 1) look.hair = v;
        else if (sscanf_s(line, "shirt=%d", &v) == 1) look.shirt = v;
        else if (sscanf_s(line, "pants=%d", &v) == 1) look.pants = v;
        else if (sscanf_s(line, "hat=%d", &v) == 1) look.hat = v;
        else if (sscanf_s(line, "acc=%d", &v) == 1) look.acc = v;
    }
    fclose(f);
    if (!has_skin_rgb && old_skin >= 0) {
        look.skin_rgb = kOldSkinRgb[clamp_look(old_skin, 0, 3)];
    }
    if (!has_hair_rgb) {
        look.hair_rgb = kOldHairRgb[clamp_look(look.hair, 0, 5)];
        look.hair = 0;
    }
    if (!has_shirt_rgb) {
        look.shirt_rgb = kOldShirtRgb[clamp_look(look.shirt, 0, 5)];
        look.shirt = 0;
    }
    if (!has_pants_rgb) {
        look.pants_rgb = kOldPantsRgb[clamp_look(look.pants, 0, 3)];
        look.pants = 0;
    }
    if (!has_under_rgb) look.undershirt_rgb = 0xA0B4D7;
    if (!has_shoes_rgb) look.shoes_rgb = 0x322A28;
    look.hair = clamp_look(look.hair, 0, 5);
    look.shirt = clamp_look(look.shirt, 0, 3);
    look.pants = clamp_look(look.pants, 0, 3);
    look.hat = clamp_look(look.hat, 0, 4);
    look.acc = clamp_look(look.acc, 0, 4);
    return look;
}

static bool write_player_cfg(const fs::path& dir, const std::string& display_name,
                             const EngineState::PlayerLook& look) {
    fs::path cfg = dir / "player.cfg";
    FILE* f = nullptr;
    if (fopen_s(&f, cfg.string().c_str(), "wb") != 0 || !f) return false;
    fprintf(f,
            "name=%s\nhair=%d\nshirt=%d\npants=%d\nhat=%d\nacc=%d\n"
            "skin_rgb=%d\nhair_rgb=%d\nshirt_rgb=%d\npants_rgb=%d\nhat_rgb=%d\nacc_rgb=%d\n"
            "undershirt_rgb=%d\nshoes_rgb=%d\n",
            display_name.c_str(), look.hair, look.shirt, look.pants, look.hat, look.acc,
            look.skin_rgb, look.hair_rgb, look.shirt_rgb, look.pants_rgb, look.hat_rgb, look.acc_rgb,
            look.undershirt_rgb, look.shoes_rgb);
    fclose(f);
    return true;
}

static int players_index_of_name(const std::string& folder) {
    for (int i = 0; i < (int)g.playerNames.size(); ++i) {
        if (g.playerNames[(size_t)i] == folder) return i;
    }
    return -1;
}

extern "C" void engine_players_refresh() {
    g.playerNames.clear();
    g.playerLooks.clear();
    std::string root = players_root_dir();
    std::error_code ec;
    fs::create_directories(root, ec);
    if (!fs::is_directory(root, ec)) return;
    for (auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        g.playerNames.push_back(entry.path().filename().string());
    }
    std::sort(g.playerNames.begin(), g.playerNames.end());
    g.playerLooks.resize(g.playerNames.size());
    for (size_t i = 0; i < g.playerNames.size(); ++i) {
        g.playerLooks[i] = read_player_look(fs::path(root) / g.playerNames[i]);
    }
}

extern "C" int engine_players_count() {
    return (int)g.playerNames.size();
}

extern "C" const char* engine_players_name(int i) {
    if (i < 0 || i >= (int)g.playerNames.size()) return "";
    g.playerScratch = g.playerNames[(size_t)i];
    return g.playerScratch.c_str();
}

extern "C" const char* engine_players_path(int i) {
    if (i < 0 || i >= (int)g.playerNames.size()) return "";
    g.playerScratch = (fs::path(players_root_dir()) / g.playerNames[(size_t)i]).string();
    return g.playerScratch.c_str();
}

extern "C" int engine_players_create_named(const char* name) {
    std::string base = sanitize_player_folder_name(name ? name : "");
    std::string root = players_root_dir();
    std::error_code ec;
    fs::create_directories(root, ec);

    std::string folder = base;
    for (int attempt = 0; attempt < 99; ++attempt) {
        fs::path path = fs::path(root) / folder;
        if (!fs::exists(path, ec)) {
            fs::create_directories(path, ec);
            if (ec) return -1;
            EngineState::PlayerLook look;
            look.hair = clamp_look(g_editHair, 0, 5);
            look.shirt = clamp_look(g_editShirt, 0, 3);
            look.pants = clamp_look(g_editPants, 0, 3);
            look.hat = clamp_look(g_editHat, 0, 4);
            look.acc = clamp_look(g_editAcc, 0, 4);
            look.skin_rgb = g_editSkinRgb;
            look.hair_rgb = g_editHairRgb;
            look.shirt_rgb = g_editShirtRgb;
            look.pants_rgb = g_editPantsRgb;
            look.hat_rgb = g_editHatRgb;
            look.acc_rgb = g_editAccRgb;
            look.undershirt_rgb = g_editUndershirtRgb;
            look.shoes_rgb = g_editShoesRgb;
            write_player_cfg(path, base, look);
            engine_players_refresh();
            int idx = players_index_of_name(folder);
            if (idx >= 0) return idx;
            return (int)g.playerNames.size() - 1;
        }
        folder = base + " " + std::to_string(attempt + 2);
    }
    return -1;
}

extern "C" int engine_players_create() {
    for (int n = 1; n <= 99; ++n) {
        std::string name = default_player_folder_name(n);
        fs::path path = fs::path(players_root_dir()) / name;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return engine_players_create_named(name.c_str());
        }
    }
    return -1;
}

extern "C" int engine_players_create_from_edit() {
    return engine_players_create_named(g_editName);
}

extern "C" void engine_edit_set_look(int hair, int shirt, int pants, int hat, int acc) {
    g_editHair = hair;
    g_editShirt = shirt;
    g_editPants = pants;
    g_editHat = hat;
    g_editAcc = acc;
}

extern "C" void engine_edit_set_look_rgb(int skin_rgb, int hair_rgb, int shirt_rgb, int pants_rgb, int hat_rgb, int acc_rgb, int undershirt_rgb, int shoes_rgb) {
    g_editSkinRgb = skin_rgb;
    g_editHairRgb = hair_rgb;
    g_editShirtRgb = shirt_rgb;
    g_editPantsRgb = pants_rgb;
    g_editHatRgb = hat_rgb;
    g_editAccRgb = acc_rgb;
    g_editUndershirtRgb = undershirt_rgb;
    g_editShoesRgb = shoes_rgb;
}

extern "C" int engine_players_look(int i, int slot) {
    if (i < 0 || i >= (int)g.playerLooks.size()) return 0;
    const EngineState::PlayerLook& look = g.playerLooks[(size_t)i];
    if (slot == 1) return look.hair;
    if (slot == 2) return look.shirt;
    if (slot == 3) return look.pants;
    if (slot == 4) return look.hat;
    if (slot == 5) return look.acc;
    return 0;
}

extern "C" int engine_players_look_rgb(int i, int slot) {
    if (i < 0 || i >= (int)g.playerLooks.size()) return 0;
    const EngineState::PlayerLook& look = g.playerLooks[(size_t)i];
    if (slot == 0) return look.skin_rgb;
    if (slot == 1) return look.hair_rgb;
    if (slot == 2) return look.shirt_rgb;
    if (slot == 3) return look.pants_rgb;
    if (slot == 4) return look.hat_rgb;
    if (slot == 5) return look.acc_rgb;
    if (slot == 6) return look.undershirt_rgb;
    if (slot == 7) return look.shoes_rgb;
    return 0;
}

extern "C" int engine_players_delete(int i) {
    if (i < 0 || i >= (int)g.playerNames.size()) return 0;
    fs::path path = fs::path(players_root_dir()) / g.playerNames[(size_t)i];
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) return 0;
    engine_players_refresh();
    return 1;
}

/* ============================================================ */
/*  Subprocess (used by hot-reload to run NexaC --dll)          */
/* ============================================================ */

extern "C" int engine_run_command(const char* cmd_line) {
    g.lastCmdOutput.clear();
    if (!cmd_line) return -1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rp = nullptr, wp = nullptr;
    if (!CreatePipe(&rp, &wp, &sa, 0)) return -1;
    SetHandleInformation(rp, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wp;
    si.hStdError  = wp;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    std::string cmd = cmd_line;
    BOOL ok = CreateProcessA(
        nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wp);
    if (!ok) {
        CloseHandle(rp);
        g.lastCmdOutput = "CreateProcess failed";
        return -1;
    }

    char buf[1024];
    DWORD got = 0;
    while (ReadFile(rp, buf, sizeof(buf), &got, nullptr) && got > 0) {
        g.lastCmdOutput.append(buf, buf + got);
    }
    CloseHandle(rp);

    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

extern "C" const char* engine_last_command_output() {
    return g.lastCmdOutput.c_str();
}

extern "C" const char* engine_exe_dir() {
    static std::string cached;
    if (!cached.empty()) return cached.c_str();
    char buf[1024];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0) { cached = "."; return cached.c_str(); }
    std::string s(buf, n);
    size_t slash = s.find_last_of("\\/");
    cached = (slash == std::string::npos) ? "." : s.substr(0, slash);
    return cached.c_str();
}

extern "C" const char* engine_engine_lib() {
    static std::string cached;
    if (!cached.empty()) return cached.c_str();
    cached = std::string(engine_exe_dir()) + "\\engine.lib";
    return cached.c_str();
}

extern "C" const char* engine_engine_dll_path() {
    static std::string cached;
    if (!cached.empty()) return cached.c_str();
    cached = std::string(engine_exe_dir()) + "\\engine.dll";
    return cached.c_str();
}

extern "C" const char* engine_runtime_exe_path() {
    static std::string cached;
    if (!cached.empty()) return cached.c_str();
    cached = std::string(engine_exe_dir()) + "\\EyescirePlayer.exe";
    return cached.c_str();
}

extern "C" const char* engine_player_dll_path() {
    static std::string cached;
    if (!cached.empty()) return cached.c_str();
    cached = std::string(engine_exe_dir()) + "\\EyescirePlayer.dll";
    return cached.c_str();
}

/* ============================================================ */
/*  DLL load + dispatch                                         */
/* ============================================================ */

namespace {

std::string normalize_path(const std::string& p) {
    std::error_code ec;
    auto abs = fs::weakly_canonical(fs::path(p), ec);
    if (ec) return p;
    return abs.string();
}

} // namespace

extern "C" int engine_dll_load(const char* path) {
    if (!path) return 0;
    std::string norm = normalize_path(path);
    auto it = g.dllByPath.find(norm);
    if (it != g.dllByPath.end()) {
        /* idempotent: return the existing handle. Use engine_dll_reload
           to force a fresh LoadLibraryA. */
        return it->second;
    }
    HMODULE m = LoadLibraryA(path);
    if (!m) return 0;
    int handle = g.nextDllHandle++;
    g.dlls[handle] = m;
    g.dllByPath[norm] = handle;
    return handle;
}

extern "C" int engine_dll_reload(const char* path) {
    if (!path) return 0;
    std::string norm = normalize_path(path);
    auto it = g.dllByPath.find(norm);
    if (it != g.dllByPath.end()) {
        engine_dll_unload(it->second);
    }
    return engine_dll_load(path);
}

extern "C" void engine_dll_unload(int handle) {
    auto it = g.dlls.find(handle);
    if (it == g.dlls.end()) return;
    if (it->second) FreeLibrary(it->second);
    g.dlls.erase(it);
    for (auto p = g.dllByPath.begin(); p != g.dllByPath.end(); ) {
        if (p->second == handle) p = g.dllByPath.erase(p);
        else                     ++p;
    }
}

extern "C" void engine_dll_unload_all() {
    for (auto& kv : g.dlls) {
        if (kv.second) FreeLibrary(kv.second);
    }
    g.dlls.clear();
    g.dllByPath.clear();
}

void engine_bp_share_push(int handle);
void engine_bp_share_pop();

extern "C" int engine_dll_call(int handle, const char* sym) {
    auto it = g.dlls.find(handle);
    if (it == g.dlls.end() || !it->second || !sym) return 0;
    typedef void (*VoidFn)();
    VoidFn fn = (VoidFn)GetProcAddress(it->second, sym);
    if (!fn) return 0;
    /* Init runs once with no object, so its Set Value has to land in the shared copy. */
    int init = std::strcmp(sym, "script_init") == 0;
    if (init) engine_bp_share_push(handle);
    fn();
    if (init) engine_bp_share_pop();
    return 1;
}

/* Button clicks call a script function with zeros. sig is a string of
   f (float), i (int/bool) and s (string), one character per argument.
   Nexa compiles float parameters as double. */

extern "C" int engine_dll_call_sig(int handle, const char* sym, const char* sig) {
    auto it = g.dlls.find(handle);
    if (it == g.dlls.end() || !it->second || !sym) return 0;
    FARPROC raw = GetProcAddress(it->second, sym);
    if (!raw) return 0;
    /* Button calls into one script share its values. Update on an object still keeps its own. */
    engine_bp_share_push(handle);
    struct SharePop { ~SharePop() { engine_bp_share_pop(); } } share_pop;
    std::string s = sig ? sig : "";
    const double z = 0.0;
    const int n = 0;
    const char* e = "";
    if (s.empty())            { ((void (*)())raw)(); return 1; }
    if (s == "f")             { ((void (*)(double))raw)(z); return 1; }
    if (s == "ff")            { ((void (*)(double, double))raw)(z, z); return 1; }
    if (s == "fff")           { ((void (*)(double, double, double))raw)(z, z, z); return 1; }
    if (s == "ffff")          { ((void (*)(double, double, double, double))raw)(z, z, z, z); return 1; }
    if (s == "i")             { ((void (*)(int))raw)(n); return 1; }
    if (s == "ii")            { ((void (*)(int, int))raw)(n, n); return 1; }
    if (s == "iii")           { ((void (*)(int, int, int))raw)(n, n, n); return 1; }
    if (s == "iiii")          { ((void (*)(int, int, int, int))raw)(n, n, n, n); return 1; }
    if (s == "s")             { ((void (*)(const char*))raw)(e); return 1; }
    if (s == "ss")            { ((void (*)(const char*, const char*))raw)(e, e); return 1; }
    if (s == "fi")            { ((void (*)(double, int))raw)(z, n); return 1; }
    if (s == "if")            { ((void (*)(int, double))raw)(n, z); return 1; }
    if (s == "ffi")           { ((void (*)(double, double, int))raw)(z, z, n); return 1; }
    if (s == "fif")           { ((void (*)(double, int, double))raw)(z, n, z); return 1; }
    if (s == "iff")           { ((void (*)(int, double, double))raw)(n, z, z); return 1; }
    ((void (*)())raw)();
    return 1;
}

/* ============================================================ */
/*  Project state                                               */
/* ============================================================ */

extern "C" void engine_project_save_manifest();

namespace {

std::string proj_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool same_rel(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i] == '\\' ? '/' : a[i];
        char cb = b[i] == '\\' ? '/' : b[i];
        if (tolower((unsigned char)ca) != tolower((unsigned char)cb)) return false;
    }
    return true;
}

/* Relative paths use forward slashes and include subfolders ("ui/button.png"). */
void proj_scan_tree(const fs::path& root,
                    const std::vector<std::string>& suffixes_lower,
                    std::vector<std::string>& out) {
    out.clear();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code rec;
        if (it->is_regular_file(rec) && !rec) {
            std::string rel = fs::relative(it->path(), root, rec).generic_string();
            if (!rec && !rel.empty()) {
                std::string lower = rel;
                for (char& c : lower) c = (char)tolower((unsigned char)c);
                for (const std::string& suf : suffixes_lower) {
                    if (ends_with(lower, suf)) { out.push_back(rel); break; }
                }
            }
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
    std::sort(out.begin(), out.end());
}

void proj_scan_dir(const std::string& dir,
                   const std::string& suffix,
                   std::vector<std::string>& out) {
    std::string suf = suffix;
    for (char& c : suf) c = (char)tolower((unsigned char)c);
    proj_scan_tree(fs::path(dir), { suf }, out);
}

/* Export ships bin/<name>.dll and leaves the .nxa source behind. A script
   still has to be found so script_init runs and can load its pictures. */
void proj_note_compiled_scripts() {
    std::vector<std::string> dlls;
    proj_scan_dir(proj_join(g.projRoot, "bin"), ".dll", dlls);
    for (const std::string& rel : dlls) {
        if (rel.size() < 5) continue;
        std::string name = rel;
        name.replace(name.size() - 4, 4, ".nxa");
        bool have = false;
        for (const std::string& s : g.projScripts) {
            if (same_rel(s, name)) { have = true; break; }
        }
        if (!have) g.projScripts.push_back(name);
    }
}

void proj_scan_dir_multi(const std::string& dir,
                         std::initializer_list<const char*> suffixes,
                         std::vector<std::string>& out) {
    std::vector<std::string> sufs;
    for (auto suf : suffixes) {
        std::string s = suf;
        for (char& c : s) c = (char)tolower((unsigned char)c);
        sufs.push_back(s);
    }
    proj_scan_tree(fs::path(dir), sufs, out);
}

/* Add any .scene files under scenes/, including subfolders, that the manifest does not list yet. */
void absorb_scene_files() {
    if (!g.projLoaded) return;
    std::vector<std::string> found;
    proj_scan_dir(proj_join(g.projRoot, "scenes"), ".scene", found);
    bool added = false;
    for (const std::string& rel : found) {
        std::string full = std::string("scenes/") + rel;
        bool have = false;
        for (const std::string& s : g.projScenes) {
            if (same_rel(s, full)) { have = true; break; }
        }
        if (!have) {
            g.projScenes.push_back(full);
            added = true;
        }
    }
    if (added) engine_project_save_manifest();
}

} // namespace

extern "C" int         engine_project_loaded() { return g.projLoaded ? 1 : 0; }
extern "C" const char* engine_project_root()   { return g.projRoot.c_str(); }
extern "C" const char* engine_project_name()   { return g.projName.c_str(); }
extern "C" void engine_project_set_name(const char* name) {
    if (!g.projLoaded) return;
    std::string clean;
    if (name) {
        for (char ch : std::string(name)) {
            unsigned char c = (unsigned char)ch;
            if (c < 32 || c == '|') continue;
            clean.push_back((char)c);
            if (clean.size() >= 64) break;
        }
    }
    while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    if (clean.empty()) clean = "Untitled";
    if (clean == g.projName) return;
    g.projName = clean;
    engine_project_save_manifest();
}

extern "C" const char* engine_project_path_join(const char* a, const char* b) {
    g.projTmp = proj_join(a ? a : "", b ? b : "");
    return g.projTmp.c_str();
}

static const char* kPrefKey[] = {
    "grid", "show_grid", "snap", "autosave", "renderer",
    "music", "sfx", "play_w", "play_h", "fullscreen", "export_used", "vsync", "hd"
};
static const int kPrefDef[] = { 32, 1, 0, 1, 33, 100, 100, 1280, 720, 0, 1, 1, 0 };
static const int kPrefCount = 13;

static int pref_clamp_key(const std::string& key, int v) {
    if (key == "grid") {
        const int steps[] = { 8, 16, 32, 64, 128 };
        int best = 32, bestD = 1000000;
        for (int n : steps) {
            int d = v - n;
            if (d < 0) d = -d;
            if (d < bestD) { bestD = d; best = n; }
        }
        return best;
    }
    if (key == "show_grid" || key == "snap" || key == "autosave" || key == "fullscreen" || key == "export_used" || key == "vsync" || key == "hd")
        return v != 0 ? 1 : 0;
    if (key == "renderer") return v == 11 ? 11 : 33;
    if (key == "music" || key == "sfx") {
        if (v < 0) return 0;
        if (v > 100) return 100;
        return v;
    }
    if (key == "play_w") {
        if (v < 640) return 640;
        if (v > 3840) return 3840;
        return v;
    }
    if (key == "play_h") {
        if (v < 360) return 360;
        if (v > 2160) return 2160;
        return v;
    }
    return v;
}

static void pref_apply_key(const std::string& key, int v) {
    if (key == "music") engine_music_set_volume(v);
    else if (key == "sfx") engine_sfx_set_volume(v);
    else if (key == "autosave") engine_set_autosave(v);
    else if (key == "renderer") {
        if (v == 11) engine_r3d_renderer("OpenGL 1.1");
        else engine_r3d_renderer("OpenGL 3.3");
    }
    else if (key == "vsync") g_vsync = v ? 1 : 0;
    else if (key == "hd") g_hd = v ? 1 : 0;
}

static void pref_write() {
    if (!g.projLoaded) return;
    std::ofstream f(proj_join(g.projRoot, "editor.cfg"), std::ios::binary);
    if (!f) return;
    f << "EYESCIRE_EDITOR 1\n";
    for (int i = 0; i < kPrefCount; ++i) {
        int v = kPrefDef[i];
        auto it = g.prefs.find(kPrefKey[i]);
        if (it != g.prefs.end()) v = it->second;
        f << kPrefKey[i] << "=" << v << "\n";
    }
}

static void pref_load() {
    g.prefs.clear();
    if (!g.projLoaded) return;
    std::ifstream f(proj_join(g.projRoot, "editor.cfg"), std::ios::binary);
    std::string line;
    if (f) {
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string key = line.substr(0, eq);
            int known = 0;
            for (int i = 0; i < kPrefCount; ++i) if (key == kPrefKey[i]) known = 1;
            if (!known) continue;
            int v = 0;
            try { v = std::stoi(line.substr(eq + 1)); } catch (...) { continue; }
            g.prefs[key] = pref_clamp_key(key, v);
        }
    }
    for (int i = 0; i < kPrefCount; ++i) {
        int v = kPrefDef[i];
        auto it = g.prefs.find(kPrefKey[i]);
        if (it != g.prefs.end()) v = it->second;
        pref_apply_key(kPrefKey[i], v);
    }
}

extern "C" int engine_pref_int(const char* key, int def) {
    if (!key) return def;
    auto it = g.prefs.find(key);
    if (it == g.prefs.end()) return def;
    return it->second;
}

extern "C" void engine_pref_set_int(const char* key, int value) {
    if (!key || !g.projLoaded) return;
    std::string k = key;
    int known = 0;
    for (int i = 0; i < kPrefCount; ++i) if (k == kPrefKey[i]) known = 1;
    if (!known) return;
    value = pref_clamp_key(k, value);
    g.prefs[k] = value;
    pref_apply_key(k, value);
    pref_write();
}

static void splash_load();
static void splash_reset();

extern "C" int engine_project_open(const char* path) {
    if (!path) return 0;
    g.projLoaded = false;
    /* Convert to an absolute path so 'recent' entries survive cwd changes
     * (the editor binary normally runs from build/, not the project root). */
    std::error_code ec;
    fs::path abs = fs::absolute(path, ec);
    if (ec) g.projRoot = path;
    else    g.projRoot = abs.string();
    g.projManifest = proj_join(g.projRoot, "project.eye");
    if (!fs::exists(g.projManifest)) return 0;
    std::ifstream f(g.projManifest);
    if (!f) return 0;
    g.projScenes.clear();
    g.projPrefabs.clear();
    g.projName = "Untitled";
    g.projStart.clear();
    std::string line;
    bool header = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("EYESCIRE_PROJECT", 0) == 0) { header = true; continue; }
        if (line.rfind("NAME ", 0) == 0)   { g.projName = line.substr(5); continue; }
        if (line.rfind("START ", 0) == 0)  { g.projStart = line.substr(6); continue; }
        if (line.rfind("SCENE ", 0) == 0)  { g.projScenes.push_back(line.substr(6)); continue; }
        if (line.rfind("PREFAB ", 0) == 0) { g.projPrefabs.push_back(line.substr(7)); continue; }
    }
    if (!header) return 0;
    proj_scan_dir(proj_join(g.projRoot, "scripts"), ".nxa", g.projScripts);
    proj_note_compiled_scripts();
    proj_scan_dir(proj_join(g.projRoot, "scripts"), ".ebp", g.projBlueprints);
    proj_scan_dir(proj_join(g.projRoot, "prefabs"), ".prefab", g.projPrefabs);
    proj_scan_dir_multi(proj_join(g.projRoot, "assets"),
                        { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".obj" },
                        g.projAssets);
    g.projActiveScene = 0;
    g.projLoaded = true;
    absorb_scene_files();
    if (!g.projStart.empty()) {
        for (int i = 0; i < (int)g.projScenes.size(); ++i) {
            if (g.projScenes[i] == g.projStart) { g.projActiveScene = i; break; }
        }
    }
    pref_load();
    splash_load();
    engine_project_recent_push(g.projRoot.c_str());
    return 1;
}

extern "C" void engine_project_close() {
    g.projLoaded = false;
    g.projRoot.clear();
    g.projName.clear();
    g.projStart.clear();
    g.prefs.clear();
    g.projManifest.clear();
    g.projScenes.clear();
    g.projScripts.clear();
    g.projBlueprints.clear();
    g.projPrefabs.clear();
    g.projAssets.clear();
    g.projActiveScene = 0;
    g_vsync = 1;
    splash_reset();
}

/* Splash screens. splash.cfg sits next to project.eye and is copied on export.
   A missing file means the built-in Eyescire screen plays and there are no
   extra images. */
struct SplashItem {
    std::string image;
    std::string holdText = "2.00";
    std::string fadeText = "0.40";
    int hold = 2000;
    int fade = 400;
};
static int g_splashBrand = 1;
static int g_splashKeys = 1;
static std::vector<SplashItem> g_splash;
static const int kSplashMax = 8;

static int splash_clamp_hold(int ms) {
    if (ms < 0) return 0;
    if (ms > 20000) return 20000;
    return ms;
}
static int splash_clamp_fade(int ms) {
    if (ms < 0) return 0;
    if (ms > 5000) return 5000;
    return ms;
}
static bool splash_parse_ms(const std::string& s, int& ms) {
    if (s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    if (v < 0) v = 0;
    ms = (int)(v * 1000.0 + 0.5);
    return true;
}
static std::string splash_fmt_ms(int ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", ms / 1000.0);
    return buf;
}
static void splash_save() {
    if (!g.projLoaded) return;
    std::ofstream f(proj_join(g.projRoot, "splash.cfg"), std::ios::binary);
    if (!f) return;
    f << "EYESCIRE_SPLASH 1\n";
    f << "brand=" << (g_splashBrand ? 1 : 0) << "\n";
    f << "keys=" << (g_splashKeys ? 1 : 0) << "\n";
    for (const SplashItem& s : g_splash) {
        std::string image = s.image;
        for (char& c : image) if (c == '|' || c == '\n' || c == '\r') c = '_';
        f << "screen " << image << "|" << s.hold << "|" << s.fade << "\n";
    }
}
static void splash_reset() {
    g_splashBrand = 1;
    g_splashKeys = 1;
    g_splash.clear();
}
static void splash_load() {
    splash_reset();
    if (!g.projLoaded) return;
    std::ifstream f(proj_join(g.projRoot, "splash.cfg"), std::ios::binary);
    if (!f) return;
    std::string line;
    bool header = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("EYESCIRE_SPLASH", 0) == 0) { header = true; continue; }
        if (!header) continue;
        if (line.rfind("brand=", 0) == 0) {
            g_splashBrand = !(line.size() > 6 && line[6] == '0');
            continue;
        }
        if (line.rfind("keys=", 0) == 0) {
            g_splashKeys = !(line.size() > 5 && line[5] == '0');
            continue;
        }
        if (line.rfind("screen ", 0) != 0 || (int)g_splash.size() >= kSplashMax) continue;
        std::string body = line.substr(7);
        auto p1 = body.rfind('|');
        if (p1 == std::string::npos || p1 == 0) continue;
        auto p0 = body.rfind('|', p1 - 1);
        if (p0 == std::string::npos) continue;
        SplashItem s;
        s.image = body.substr(0, p0);
        int hold = 2000, fade = 400;
        try { hold = std::stoi(body.substr(p0 + 1, p1 - p0 - 1)); } catch (...) {}
        try { fade = std::stoi(body.substr(p1 + 1)); } catch (...) {}
        s.hold = splash_clamp_hold(hold);
        s.fade = splash_clamp_fade(fade);
        s.holdText = splash_fmt_ms(s.hold);
        s.fadeText = splash_fmt_ms(s.fade);
        if (!s.image.empty()) g_splash.push_back(std::move(s));
    }
}

extern "C" int engine_splash_brand() { return g_splashBrand ? 1 : 0; }
extern "C" void engine_splash_set_brand(int on) {
    if (!g.projLoaded) return;
    g_splashBrand = on ? 1 : 0;
    splash_save();
}
extern "C" int engine_splash_keys() { return g_splashKeys ? 1 : 0; }
extern "C" void engine_splash_set_keys(int on) {
    if (!g.projLoaded) return;
    g_splashKeys = on ? 1 : 0;
    splash_save();
}
extern "C" int engine_splash_count() { return (int)g_splash.size(); }
extern "C" const char* engine_splash_image(int i) {
    if (i < 0 || i >= (int)g_splash.size()) return "";
    return g_splash[i].image.c_str();
}
extern "C" const char* engine_splash_hold_text(int i) {
    if (i < 0 || i >= (int)g_splash.size()) return "";
    return g_splash[i].holdText.c_str();
}
extern "C" const char* engine_splash_fade_text(int i) {
    if (i < 0 || i >= (int)g_splash.size()) return "";
    return g_splash[i].fadeText.c_str();
}
extern "C" void engine_splash_set_hold_text(int i, const char* text) {
    if (!g.projLoaded || i < 0 || i >= (int)g_splash.size()) return;
    g_splash[i].holdText = text ? text : "";
    int ms = 0;
    if (splash_parse_ms(g_splash[i].holdText, ms)) g_splash[i].hold = splash_clamp_hold(ms);
    splash_save();
}
extern "C" void engine_splash_set_fade_text(int i, const char* text) {
    if (!g.projLoaded || i < 0 || i >= (int)g_splash.size()) return;
    g_splash[i].fadeText = text ? text : "";
    int ms = 0;
    if (splash_parse_ms(g_splash[i].fadeText, ms)) g_splash[i].fade = splash_clamp_fade(ms);
    splash_save();
}
extern "C" int engine_splash_add(const char* image, int hold_ms, int fade_ms) {
    if (!g.projLoaded || !image || !*image) return -1;
    if ((int)g_splash.size() >= kSplashMax) return -1;
    SplashItem s;
    s.image = image;
    for (char& c : s.image) if (c == '|' || c == '\n' || c == '\r') c = '_';
    s.hold = splash_clamp_hold(hold_ms);
    s.fade = splash_clamp_fade(fade_ms);
    s.holdText = splash_fmt_ms(s.hold);
    s.fadeText = splash_fmt_ms(s.fade);
    g_splash.push_back(std::move(s));
    splash_save();
    return (int)g_splash.size() - 1;
}
extern "C" void engine_splash_remove(int i) {
    if (!g.projLoaded || i < 0 || i >= (int)g_splash.size()) return;
    g_splash.erase(g_splash.begin() + i);
    splash_save();
}

static std::string splash_logo_path() {
    std::error_code ec;
    std::string beside = std::string(engine_exe_dir()) + "\\icons\\eyescire.png";
    if (fs::exists(beside, ec)) return beside;
    fs::path up = fs::path(engine_exe_dir()) / ".." / "icons" / "eyescire.png";
    fs::path canon = fs::weakly_canonical(up, ec);
    if (!ec && fs::exists(canon, ec)) return canon.string();
    return "";
}
static bool splash_pressed() {
    if (!g_splashKeys) return false;
    if (g.mousePressed[0] || g.mousePressed[1] || g.mousePressed[2]) return true;
    for (int i = 0; i < 256; ++i) if (g.keyPressed[i]) return true;
    return false;
}
static void splash_fit(int iw, int ih, int vw, int vh, int& x, int& y, int& w, int& h) {
    int mx = vw / 8;
    int my = vh / 7;
    if (mx < 36) mx = 36;
    if (my < 36) my = 36;
    int maxw = vw - mx * 2;
    int maxh = vh - my * 2;
    if (maxw < 8) maxw = 8;
    if (maxh < 8) maxh = 8;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    w = maxw;
    h = maxw * ih / iw;
    if (h > maxh) {
        h = maxh;
        w = maxh * iw / ih;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    x = (vw - w) / 2;
    y = (vh - h) / 2;
}

/* The card is drawn at full strength. A black veil on top is what fades it,
   so the type, the logo, and a picture all dim together. */
static void splash_stage(int vw, int vh) {
    Gdiplus::Graphics gfx(g.backDC);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::LinearGradientBrush bg(
        Gdiplus::Point(0, 0), Gdiplus::Point(0, vh),
        Gdiplus::Color(255, 18, 22, 34),
        Gdiplus::Color(255, 8, 9, 14));
    gfx.FillRectangle(&bg, 0, 0, vw, vh);
    int glow = (vw < vh ? vw : vh) * 2 / 3;
    Gdiplus::GraphicsPath path;
    path.AddEllipse((vw - glow) / 2, (vh - glow) / 2 - vh / 18, glow, glow);
    Gdiplus::PathGradientBrush light(&path);
    light.SetCenterColor(Gdiplus::Color(56, 72, 112, 176));
    Gdiplus::Color edge(0, 18, 22, 34);
    INT n = 1;
    light.SetSurroundColors(&edge, &n);
    gfx.FillEllipse(&light, (vw - glow) / 2, (vh - glow) / 2 - vh / 18, glow, glow);
}

static void splash_veil(int vw, int vh, int vis) {
    int a = 255 - vis;
    if (a <= 0) return;
    engine_fill_rect_a(0, 0, vw, vh, 0, 0, 0, a);
}

static void splash_use_face(const char* face, int px) {
    engine_set_font_bold(0);
    engine_set_font(face, px);
}

static void splash_restore_face(const std::string& face, int px, int bold) {
    engine_set_font(face.c_str(), px);
    engine_set_font_bold(bold);
}

/* One line, centered on x, with a little space between letters. */
static int splash_tracked(int cx, int y, const char* msg, int gap, int r, int g_, int b) {
    int total = 0;
    for (const char* p = msg; *p; ++p) {
        char s[2] = { *p, 0 };
        total += engine_text_width(s);
        if (p[1]) total += (*p == ' ') ? gap * 3 : gap;
    }
    int x = cx - total / 2;
    for (const char* p = msg; *p; ++p) {
        char s[2] = { *p, 0 };
        engine_text(x, y, s, r, g_, b);
        x += engine_text_width(s);
        if (p[1]) x += (*p == ' ') ? gap * 3 : gap;
    }
    return total;
}

static void splash_draw_brand(int vw, int vh, int logoId) {
    const char* msg = "Made with Eyescire Engine";
    int side = vw < vh ? vw : vh;
    int px = side / 28;
    if (px < 18) px = 18;
    if (px > 56) px = 56;
    std::string face = g.fontFace;
    int size = g.fontSize;
    int bold = g.fontBold;
    splash_use_face("Segoe UI", px);
    int th = engine_text_height();
    int logo = 0;
    if (logoId) {
        logo = side / 4;
        if (logo < 96) logo = 96;
        int logoCap = engine_image_w(logoId);
        if (logoCap < 240) logoCap = 240;
        if (logoCap > 640) logoCap = 640;
        if (logo > logoCap) logo = logoCap;
    }
    int rule = 2;
    int gap = px / 2;
    int block = th + gap + rule + gap + logo;
    int top = (vh - block) / 2;
    if (top < 16) top = 16;
    splash_tracked(vw / 2, top, msg, px / 14 > 1 ? px / 14 : 1, 226, 230, 238);
    int rw = side / 10;
    if (rw < 48) rw = 48;
    if (rw > 120) rw = 120;
    engine_fill_rect(vw / 2 - rw / 2, top + th + gap, rw, rule, 126, 156, 196);
    if (logoId) {
        int lx = (vw - logo) / 2;
        int ly = top + th + gap + rule + gap;
        engine_fill_round_a(lx + 8, ly + logo / 10, logo, logo, logo / 7, 0, 0, 0, 90);
        engine_image_blit_scaled_rot_a(logoId, lx, ly, logo, logo, 0, 255);
    }
    splash_restore_face(face, size, bold);
}

static void splash_draw_picture(int id, int vw, int vh) {
    int x = 0, y = 0, w = 1, h = 1;
    splash_fit(engine_image_w(id), engine_image_h(id), vw, vh, x, y, w, h);
    int m = 14;
    engine_fill_round_a(x - m + 6, y - m + 10, w + m * 2, h + m * 2, 10, 0, 0, 0, 110);
    engine_fill_rect(x - 1, y - 1, w + 2, h + 2, 46, 54, 70);
    engine_image_blit_scaled_rot_a(id, x, y, w, h, 0, 255);
}

extern "C" int engine_splash_run() {
    if (!g.hwnd || !g.backDC || !g.projLoaded) return 1;
    struct Step { int brand; std::string image; int hold; int fade; };
    std::vector<Step> steps;
    if (g_splashBrand) steps.push_back(Step{ 1, "", 2200, 450 });
    for (const SplashItem& s : g_splash) {
        if (!s.image.empty()) steps.push_back(Step{ 0, s.image, s.hold, s.fade });
    }
    if (steps.empty()) return 1;

    view_hole(0, 0, 0, 0, 0);
    /* Fullscreen leaves the game picture at the play size and stretches it.
       Draw the splash at the monitor size, then put the game picture back. */
    update_display_layout();
    int savedW = g.width;
    int savedH = g.height;
    if (g.clientW >= 320 && g.clientH >= 240) engine_fit_buffer(g.clientW, g.clientH);
    int logoId = 0;
    std::string logo = splash_logo_path();
    if (!logo.empty()) logoId = engine_image_load(logo.c_str());
    std::vector<int> ids(steps.size(), 0);
    for (int i = 0; i < (int)steps.size(); ++i) {
        if (!steps[i].brand) ids[i] = engine_image_load(steps[i].image.c_str());
    }

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    int index = 0;
    int phase = 0;
    double acc = 0;
    double arm = 0.3;
    int alive = 1;
    while (alive && index < (int)steps.size()) {
        if (!engine_pump()) { alive = 0; break; }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = freq.QuadPart ? (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart : 0.016;
        prev = now;
        if (dt < 0) dt = 0;
        if (dt > 0.05) dt = 0.05;
        acc += dt;
        if (arm > 0) arm -= dt;
        const Step& st = steps[index];
        double fade = st.fade / 1000.0;
        double hold = st.hold / 1000.0;
        if (arm <= 0 && splash_pressed()) {
            ++index;
            phase = 0;
            acc = 0;
            continue;
        }
        if (phase == 0 && acc >= fade) { phase = 1; acc = 0; }
        else if (phase == 1 && acc >= hold) { phase = 2; acc = 0; }
        else if (phase == 2 && acc >= fade) {
            ++index;
            phase = 0;
            acc = 0;
            continue;
        }
        int vis = 255;
        if (phase == 0) vis = (fade <= 0.0) ? 255 : (int)(acc / fade * 255.0);
        else if (phase == 2) vis = (fade <= 0.0) ? 0 : (int)((1.0 - acc / fade) * 255.0);
        if (vis < 0) vis = 0;
        if (vis > 255) vis = 255;
        int vw = g.width < 8 ? 8 : g.width;
        int vh = g.height < 8 ? 8 : g.height;
        splash_stage(vw, vh);
        if (st.brand) splash_draw_brand(vw, vh, logoId);
        else if (ids[index] != 0) splash_draw_picture(ids[index], vw, vh);
        else {
            std::string face = g.fontFace;
            int size = g.fontSize;
            int bold = g.fontBold;
            splash_use_face("Segoe UI", 22);
            int tw = engine_text_width(st.image.c_str());
            engine_text((vw - tw) / 2, vh / 2 - engine_text_height() / 2, st.image.c_str(), 200, 206, 216);
            splash_restore_face(face, size, bold);
        }
        splash_veil(vw, vh, vis);
        engine_present();
        if (!g_vsync && dt < 0.016) Sleep((DWORD)((0.016 - dt) * 1000.0));
    }
    if (logoId) engine_image_free(logoId);
    for (int id : ids) if (id) engine_image_free(id);
    if (g.width != savedW || g.height != savedH) engine_fit_buffer(savedW, savedH);
    for (int i = 0; i < 256; ++i) g.keyPressed[i] = false;
    for (int i = 0; i < 3; ++i) g.mousePressed[i] = false;
    return alive;
}

extern "C" void engine_project_save_manifest() {
    if (!g.projLoaded) return;
    std::ofstream f(g.projManifest, std::ios::binary);
    if (!f) return;
    f << "EYESCIRE_PROJECT 1\n";
    f << "NAME " << g.projName << "\n";
    if (!g.projStart.empty()) f << "START " << g.projStart << "\n";
    for (auto& s : g.projScenes)  f << "SCENE " << s << "\n";
    for (auto& p : g.projPrefabs) f << "PREFAB " << p << "\n";
}

static void proj_write_template_topdown(const std::string& root) {
    std::ofstream sc(proj_join(root, "scenes\\main.scene"), std::ios::binary);
    sc << "EYESCIRE_SCENE 2\nNEXT_ID 4\n"
       << "ENTITY 1|player|rect|960|660|108|108|220|180|80||||player.nxa\n"
       << "ENTITY 2|coin_a|rect|480|420|72|72|240|210|80||coin||\n"
       << "ENTITY 3|coin_b|rect|1560|960|72|72|240|210|80||coin||\n";

    std::ofstream pl(proj_join(root, "scripts\\player.nxa"), std::ios::binary);
    pl <<
"// Topdown player: WASD/arrow movement, picks up coins on touch.\n"
"#include <eyescire/script>\n\n"
"let speed: float = 660.0;\n\n"
"fn script_init() {\n"
"    eng_log(\"[player] ready\");\n"
"}\n\n"
"fn script_update() {\n"
"    let me: int = eng_self();\n"
"    if (me < 0) return;\n"
"    let dt: float = eng_dt_seconds();\n"
"    let step: float = speed * dt;\n"
"    let dx: int = 0;\n"
"    let dy: int = 0;\n"
"    if (key_down(VK_A) || key_down(VK_LEFT))  dx = dx - 1;\n"
"    if (key_down(VK_D) || key_down(VK_RIGHT)) dx = dx + 1;\n"
"    if (key_down(VK_W) || key_down(VK_UP))    dy = dy - 1;\n"
"    if (key_down(VK_S) || key_down(VK_DOWN))  dy = dy + 1;\n"
"    if (dx != 0 || dy != 0) {\n"
"        let mx: float = dx;\n"
"        let my: float = dy;\n"
"        let len: float = math.sqrt(mx * mx + my * my);\n"
"        let vx: int = (mx / len) * step;\n"
"        let vy: int = (my / len) * step;\n"
"        ent_set_x(me, ent_x(me) + vx);\n"
"        ent_set_y(me, ent_y(me) + vy);\n"
"    }\n"
"    let coin: int = ent_first_overlap(me, \"coin\");\n"
"    if (coin >= 0) {\n"
"        eng_log(\"[player] picked up \" + ent_name(coin));\n"
"        ent_destroy(coin);\n"
"    }\n"
"}\n";
}

/* 3D starter at 3840x2160. World, camera, lights, and physics are 3x the old
 * 720p layout. The hint label stays in 1280x720 UI units so the words stay
 * a normal size. The camera looks down 18 degrees; Box2D runs on x/y. */
static void proj_write_template_3d(const std::string& root) {
    std::ofstream sc(proj_join(root, "scenes\\main.scene"), std::ios::binary);
    /* id|name|kind|x|y|w|h|r|g|b|sprite|text|script|parent|rot|cam|
       body|btype|dens|fric|rest|gscale|fixed|z|depth|model|
       light intensity|range|cone angle|tilt */
    sc << "EYESCIRE_SCENE 8\nNEXT_ID 11\nGRAVITY 0 2940\nPPM 150\nRENDER 3D\nSKY solid\n"
       << "ENTITY 9|Sun|light_directional|180|360|144|144|255|244|214||||0|335|0|0|1|1|0.3|0|1|0|0|0||60|1200|45|35\n"
       << "ENTITY 10|Lamp|light_point|2568|900|144|144|255|170|90||||0|0|0|0|1|1|0.3|0|1|0|-180|0||160|1260|45|0\n"
       << "ENTITY 1|Main Camera|camera|0|0|3840|2160|90|200|120||||0|18|1|0|1|1|0.3|0|1|0|0|0|\n"
       << "ENTITY 2|ground|cube|240|1800|3360|120|70|90|120||||0|0|0|1|0|1|0.8|0|1|0|-750|1500|\n"
       << "ENTITY 3|ledge|cube|2280|1320|720|90|90|110|150||||0|0|0|1|0|1|0.8|0|1|0|-180|360|\n"
       << "ENTITY 4|player|cube|900|1440|168|168|79|140|255|||player.nxa|0|0|0|1|1|1|0.2|0|1|1|-84|0|\n"
       << "ENTITY 5|coin_a|sphere|1560|1560|108|108|245|200|70||||0|0|0|0|1|1|0.3|0|1|0|-54|0|\n"
       << "ENTITY 6|coin_b|sphere|2580|1140|108|108|245|200|70||||0|0|0|0|1|1|0.3|0|1|0|-54|0|\n"
       << "ENTITY 7|pillar|cylinder|3240|1260|180|540|110|210|150||||0|0|0|1|0|1|0.3|0|1|0|-300|0|\n"
       << "ENTITY 8|hint|label|40|30|480|26|230|232|240||A/D move, Space jumps, grab the coins||0|0|0|0|1|1|0.3|0|1|0|0|0|\n";

    std::ofstream pl(proj_join(root, "scripts\\player.nxa"), std::ios::binary);
    pl <<
"// 3D platformer player: A/D or arrows move, Space/W/Up jumps, coins vanish on touch.\n"
"// Scene is 3840x2160, so speeds are in those pixels. Physics is Box2D on x/y.\n"
"#include <eyescire/script>\n\n"
"let speed: float = 780.0;\n"
"let jump: float = 1680.0;\n\n"
"fn script_init() {\n"
"    eng_log(\"[player] ready\");\n"
"}\n\n"
"fn script_update() {\n"
"    let me: int = eng_self();\n"
"    if (me < 0) return;\n"
"    let vx: float = 0.0;\n"
"    if (key_down(VK_A) || key_down(VK_LEFT))  vx = vx - speed;\n"
"    if (key_down(VK_D) || key_down(VK_RIGHT)) vx = vx + speed;\n"
"    let vy: float = ent_velocity_y(me);\n"
"    let grounded: int = 0;\n"
"    if (vy > -5.0 && vy < 5.0) grounded = 1;\n"
"    if (grounded == 1 && (key_pressed(VK_SPACE) || key_pressed(VK_W) || key_pressed(VK_UP))) vy = 0.0 - jump;\n"
"    ent_set_velocity(me, vx, vy);\n"
"    let coin: int = ent_first_overlap(me, \"coin\");\n"
"    if (coin >= 0) {\n"
"        eng_log(\"[player] coin!\");\n"
"        ent_destroy(coin);\n"
"    }\n"
"}\n";
}

static void ebp_w32(std::ostream& o, uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }
static void ebp_wi(std::ostream& o, int v) { o.write(reinterpret_cast<const char*>(&v), 4); }
static void ebp_wf(std::ostream& o, float v) { o.write(reinterpret_cast<const char*>(&v), 4); }
static void ebp_ws(std::ostream& o, const char* s) {
    uint32_t n = 0;
    if (s) while (s[n]) ++n;
    ebp_w32(o, n);
    if (n) o.write(s, n);
}

struct EbpN {
    int kind, x, y, op;
    float a, b;
    const char* text;
    const char* name;
};

static void ebp_save(const std::string& path, const EbpN* nodes, int nc,
                     const int* links, int lc, const char* var, float var0) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return;
    o.write("EBP1", 4);
    ebp_w32(o, 1);
    ebp_wi(o, 40);
    ebp_wi(o, 40);
    ebp_w32(o, (uint32_t)nc);
    for (int i = 0; i < nc; ++i) {
        const EbpN& n = nodes[i];
        ebp_w32(o, (uint32_t)n.kind);
        ebp_wi(o, n.x); ebp_wi(o, n.y); ebp_wi(o, n.op);
        ebp_wf(o, n.a); ebp_wf(o, n.b);
        ebp_ws(o, n.text); ebp_ws(o, n.name);
    }
    ebp_w32(o, (uint32_t)lc);
    for (int i = 0; i < lc; ++i) {
        ebp_w32(o, (uint32_t)links[i * 4]);
        ebp_w32(o, (uint32_t)links[i * 4 + 1]);
        ebp_w32(o, (uint32_t)links[i * 4 + 2]);
        ebp_w32(o, (uint32_t)links[i * 4 + 3]);
    }
    ebp_w32(o, 0);
    if (var && var[0]) {
        ebp_w32(o, 1);
        ebp_ws(o, var);
        ebp_wi(o, 2);
        ebp_wf(o, var0);
        ebp_ws(o, "");
    } else {
        ebp_w32(o, 0);
    }
}

static void proj_write_template_empty(const std::string& root) {
    std::ofstream sc(proj_join(root, "scenes\\main.scene"), std::ios::binary);
    sc << "EYESCIRE_SCENE 2\nNEXT_ID 1\n";
}

extern "C" int engine_project_create(const char* path, const char* name, const char* template_id) {
    if (!path || !name) return 0;
    std::error_code ec;
    fs::path abs = fs::absolute(path, ec);
    std::string root = ec ? std::string(path) : abs.string();
    fs::create_directories(root, ec);
    fs::create_directories(proj_join(root, "scenes"), ec);
    fs::create_directories(proj_join(root, "scripts"), ec);
    fs::create_directories(proj_join(root, "assets"), ec);
    fs::create_directories(proj_join(root, "bin"), ec);
    fs::create_directories(proj_join(root, "prefabs"), ec);

    std::string manifest = proj_join(root, "project.eye");
    {
        std::ofstream f(manifest, std::ios::binary);
        f << "EYESCIRE_PROJECT 1\nNAME " << name << "\nSCENE scenes/main.scene\n";
    }

    std::string tpl = template_id ? template_id : "empty";
    if (tpl == "topdown")  proj_write_template_topdown(root);
    else if (tpl == "3d")  proj_write_template_3d(root);
    else                   proj_write_template_empty(root);

    {
        std::ofstream cfg(proj_join(root, "editor.cfg"), std::ios::binary);
        cfg << "EYESCIRE_EDITOR 1\nplay_w=1280\nplay_h=720\n";
    }

    return 1;
}

extern "C" int engine_project_scene_count() { return (int)g.projScenes.size(); }
extern "C" const char* engine_project_scene_rel(int i) {
    if (i < 0 || i >= (int)g.projScenes.size()) return "";
    return g.projScenes[i].c_str();
}
extern "C" const char* engine_project_scene_abs(int i) {
    if (i < 0 || i >= (int)g.projScenes.size()) return "";
    g.projTmp = proj_join(g.projRoot, g.projScenes[i]);
    return g.projTmp.c_str();
}
extern "C" const char* engine_project_start_scene() {
    if (!g.projStart.empty()) return g.projStart.c_str();
    if (!g.projScenes.empty()) return g.projScenes[0].c_str();
    return "";
}
extern "C" void engine_project_set_start_scene(const char* rel) {
    if (!g.projLoaded || !rel) return;
    std::string s = rel;
    int found = 0;
    for (const std::string& scene : g.projScenes) if (scene == s) found = 1;
    if (!found) return;
    if (s == g.projStart) return;
    g.projStart = s;
    engine_project_save_manifest();
}
extern "C" int engine_project_active_scene() { return g.projActiveScene; }
extern "C" void engine_project_set_active_scene(int i) {
    if (i >= 0 && i < (int)g.projScenes.size()) g.projActiveScene = i;
}
extern "C" int engine_project_add_scene(const char* rel) {
    if (!rel || !g.projLoaded) return -1;
    g.projScenes.emplace_back(rel);
    engine_project_save_manifest();
    return (int)g.projScenes.size() - 1;
}

extern "C" int engine_project_remove_scene(int i) {
    if (!g.projLoaded) return 0;
    if (i < 0 || i >= (int)g.projScenes.size()) return 0;
    if (g.projScenes.size() <= 1) return 0;
    g.projScenes.erase(g.projScenes.begin() + i);
    if (g.projActiveScene >= (int)g.projScenes.size())
        g.projActiveScene = (int)g.projScenes.size() - 1;
    else if (g.projActiveScene > i)
        g.projActiveScene--;
    engine_project_save_manifest();
    return 1;
}

extern "C" int engine_project_script_count() { return (int)g.projScripts.size(); }
extern "C" const char* engine_project_script_name(int i) {
    if (i < 0 || i >= (int)g.projScripts.size()) return "";
    return g.projScripts[i].c_str();
}
extern "C" const char* engine_project_script_path(int i) {
    if (i < 0 || i >= (int)g.projScripts.size()) return "";
    g.projTmp = proj_join(proj_join(g.projRoot, "scripts"), g.projScripts[i]);
    return g.projTmp.c_str();
}
extern "C" const char* engine_project_script_dll(int i) {
    if (i < 0 || i >= (int)g.projScripts.size()) return "";
    std::string base = g.projScripts[i];
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".nxa") == 0) {
        base.replace(base.size() - 4, 4, ".dll");
    } else {
        base += ".dll";
    }
    g.projTmp = proj_join(proj_join(g.projRoot, "bin"), base);
    return g.projTmp.c_str();
}
extern "C" void engine_project_rescan_scripts() {
    if (!g.projLoaded) return;
    proj_scan_dir(proj_join(g.projRoot, "scripts"), ".nxa", g.projScripts);
    proj_note_compiled_scripts();
    proj_scan_dir(proj_join(g.projRoot, "scripts"), ".ebp", g.projBlueprints);
}

extern "C" void engine_project_rescan_scenes() {
    absorb_scene_files();
}

extern "C" int engine_project_prefab_count() { return (int)g.projPrefabs.size(); }
extern "C" const char* engine_project_prefab_name(int i) {
    if (i < 0 || i >= (int)g.projPrefabs.size()) return "";
    return g.projPrefabs[i].c_str();
}
extern "C" const char* engine_project_prefab_path(int i) {
    if (i < 0 || i >= (int)g.projPrefabs.size()) return "";
    g.projTmp = proj_join(proj_join(g.projRoot, "prefabs"), g.projPrefabs[i]);
    return g.projTmp.c_str();
}

extern "C" void engine_project_rescan_prefabs() {
    if (!g.projLoaded) return;
    proj_scan_dir(proj_join(g.projRoot, "prefabs"), ".prefab", g.projPrefabs);
}

extern "C" int engine_project_save_prefab(const char* name, int slot) {
    if (!name || !g.projLoaded || !active_slot(slot)) return 0;
    std::string fname = name;
    if (!ends_with(fname, ".prefab")) fname += ".prefab";
    std::string dir = proj_join(g.projRoot, "prefabs");
    fs::create_directories(dir);
    std::string full = proj_join(dir, fname);
    std::ofstream f(full, std::ios::binary);
    if (!f) return 0;
    const Entity& e = g.entities[slot];
    f << "EYESCIRE_PREFAB 1\n";
    f << "PREFAB " << e.kind << "|" << e.name << "|"
      << e.w << "|" << e.h << "|"
      << e.r << "|" << e.g << "|" << e.b << "|"
      << e.sprite << "|" << e.text << "|" << e.script << "|"
      << e.depth << "|" << e.model << "|" << e.a << "|"
      << e.btnScript << "|" << e.btnFn << "|" << e.hitbox << "|" << e.uiValue << "|"
      << e.sound << "|" << (e.soundLoop ? 1 : 0) << "|" << e.soundRange << "|"
      << e.emitRate << "|" << e.emitLife << "|" << e.emitSpeed << "|"
      << e.emitSpread << "|" << e.emitSize << "|" << (e.emitHD ? 1 : 0) << "\n";

    auto it = std::find(g.projPrefabs.begin(), g.projPrefabs.end(), fname);
    if (it == g.projPrefabs.end()) {
        g.projPrefabs.push_back(fname);
        std::sort(g.projPrefabs.begin(), g.projPrefabs.end());
    }
    return 1;
}

extern "C" int engine_project_spawn_prefab(int i, int x, int y) {
    if (i < 0 || i >= (int)g.projPrefabs.size()) return -1;
    std::string full = proj_join(proj_join(g.projRoot, "prefabs"), g.projPrefabs[i]);
    std::ifstream f(full, std::ios::binary);
    if (!f) return -1;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("PREFAB ", 0) == 0) {
            auto parts = split_pipe(line.substr(7));
            if (parts.size() < 7) return -1;
            std::string kind = parts[0];
            std::string name = parts[1];
            int w = 32, h = 32, r = 220, g_ = 220, b = 220;
            try { w = std::stoi(parts[2]); } catch (...) {}
            try { h = std::stoi(parts[3]); } catch (...) {}
            try { r = std::stoi(parts[4]); } catch (...) {}
            try { g_ = std::stoi(parts[5]); } catch (...) {}
            try { b = std::stoi(parts[6]); } catch (...) {}
            int slot = engine_entity_create(kind.c_str(), name.c_str(),
                                            x - w / 2, y - h / 2, w, h, r, g_, b);
            if (slot >= 0) {
                if (parts.size() >= 8 && !parts[7].empty()) engine_entity_set_sprite(slot, parts[7].c_str());
                if (parts.size() >= 9 && !parts[8].empty()) engine_entity_set_text(slot, parts[8].c_str());
                if (parts.size() >= 10 && !parts[9].empty()) engine_entity_set_script(slot, parts[9].c_str());
                if (parts.size() >= 11) { try { engine_entity_set_depth(slot, std::stoi(parts[10])); } catch (...) {} }
                if (parts.size() >= 12 && !parts[11].empty()) engine_entity_set_model(slot, parts[11].c_str());
                if (parts.size() >= 13) {
                    try { engine_entity_set_alpha(slot, std::stoi(parts[12])); } catch (...) {}
                }
                if (parts.size() >= 14 && !parts[13].empty()) engine_entity_set_button_script(slot, parts[13].c_str());
                if (parts.size() >= 15 && !parts[14].empty()) engine_entity_set_button_fn(slot, parts[14].c_str());
                if (parts.size() >= 16) {
                    int hb = 0;
                    try { hb = std::stoi(parts[15]); } catch (...) {}
                    engine_entity_set_hitbox(slot, hb);
                }
                if (parts.size() >= 17) {
                    int uv = 0;
                    try { uv = std::stoi(parts[16]); } catch (...) {}
                    engine_entity_set_ui_value(slot, uv);
                }
                if (parts.size() >= 18 && !parts[17].empty()) engine_entity_set_sound(slot, parts[17].c_str());
                if (parts.size() >= 19) { try { engine_entity_set_sound_loop(slot, std::stoi(parts[18])); } catch (...) {} }
                if (parts.size() >= 20) { try { engine_entity_set_sound_range(slot, std::stoi(parts[19])); } catch (...) {} }
                if (parts.size() >= 21) { try { engine_entity_set_emit_rate(slot, std::stoi(parts[20])); } catch (...) {} }
                if (parts.size() >= 22) { try { engine_entity_set_emit_life(slot, std::stoi(parts[21])); } catch (...) {} }
                if (parts.size() >= 23) { try { engine_entity_set_emit_speed(slot, std::stoi(parts[22])); } catch (...) {} }
                if (parts.size() >= 24) { try { engine_entity_set_emit_spread(slot, std::stoi(parts[23])); } catch (...) {} }
                if (parts.size() >= 25) { try { engine_entity_set_emit_size(slot, std::stoi(parts[24])); } catch (...) {} }
                if (parts.size() >= 26) { try { engine_entity_set_emit_hd(slot, std::stoi(parts[25])); } catch (...) {} }
            }
            return slot;
        }
    }
    return -1;
}

/* ----- assets (images in <project>/assets) -------------------------------- */

extern "C" int engine_project_asset_count() { return (int)g.projAssets.size(); }

extern "C" const char* engine_project_asset_name(int i) {
    if (i < 0 || i >= (int)g.projAssets.size()) return "";
    return g.projAssets[i].c_str();
}

extern "C" const char* engine_project_asset_path(int i) {
    if (i < 0 || i >= (int)g.projAssets.size()) return "";
    g.projTmp = proj_join(proj_join(g.projRoot, "assets"), g.projAssets[i]);
    return g.projTmp.c_str();
}

extern "C" const char* engine_project_asset_rel(int i) {
    if (i < 0 || i >= (int)g.projAssets.size()) return "";
    g.projTmp = std::string("assets/") + g.projAssets[i];
    return g.projTmp.c_str();
}

extern "C" void engine_project_rescan_assets() {
    if (!g.projLoaded) return;
    proj_scan_dir_multi(proj_join(g.projRoot, "assets"),
                        { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".obj" },
                        g.projAssets);
}

static std::string g_lastImport;

extern "C" int engine_project_import_asset(const char* src_path) {
    if (!g.projLoaded || !src_path) return 0;
    std::error_code ec;
    fs::path src(src_path);
    if (!fs::exists(src, ec)) return 0;
    fs::path assets = fs::path(g.projRoot) / "assets";
    fs::path srcAbs = fs::weakly_canonical(src, ec);
    fs::path assetsAbs = fs::weakly_canonical(assets, ec);
    if (!ec) {
        fs::path rel = fs::relative(srcAbs, assetsAbs, ec);
        std::string rels = rel.generic_string();
        if (!ec && !rels.empty() && rels != "." && rels.rfind("..", 0) != 0) {
            g_lastImport = std::string("assets/") + rels;
            return 1;
        }
    }
    fs::path dstDir = assets;
    fs::create_directories(dstDir, ec);
    fs::path dst = dstDir / src.filename();
    /* if a same-named asset exists, append _1, _2, ... */
    if (fs::exists(dst, ec)) {
        std::string stem = dst.stem().string();
        std::string ext  = dst.extension().string();
        int i = 1;
        while (fs::exists(dst, ec)) {
            dst = dstDir / (stem + "_" + std::to_string(i) + ext);
            ++i;
        }
    }
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return 0;
    g_lastImport = std::string("assets/") + dst.filename().string();
    engine_project_rescan_assets();
    return 1;
}

extern "C" const char* engine_project_last_import() {
    return g_lastImport.c_str();
}

/* Files a build actually reaches. The launch scene is the seed. A scene, script,
   prefab, model, or config is then read, and any project file named in it is
   kept. Scripts that load a scene or spawn a prefab by number keep the whole
   list, because those numbers depend on every file still being there. */

struct ExportCat {
    std::string rel;
    std::string key;
};

static std::vector<ExportCat> g_xcat;
static std::unordered_map<std::string, int> g_xby;
static std::vector<int> g_xqueue;
static std::vector<std::string> g_exportUsed;
static std::unordered_set<std::string> g_exportKeys;
static int g_exportStart = -1;
static int g_xwantScenes = 0;
static int g_xwantPrefabs = 0;

struct XNeedle {
    std::string text;
    int cat = 0;
};
static std::vector<XNeedle> g_xneedles;

static std::string export_norm(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    while (!s.empty() && (s.front() == '/' || s.front() == ' ')) s.erase(s.begin());
    return s;
}

static std::string export_ext(const std::string& key) {
    auto dot = key.rfind('.');
    auto slash = key.rfind('/');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    return key.substr(dot);
}

static bool export_tok_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static bool export_hay_has(const std::string& hay, const std::string& needle) {
    if (needle.size() < 5) return false;
    size_t pos = 0;
    while (pos < hay.size()) {
        pos = hay.find(needle, pos);
        if (pos == std::string::npos) return false;
        bool left = pos == 0 || !export_tok_char(hay[pos - 1]);
        size_t end = pos + needle.size();
        bool right = end == hay.size() || !export_tok_char(hay[end]);
        if (left && right) return true;
        pos += 1;
    }
    return false;
}

/* '_' is a break, so proj_spawn_prefab still counts as spawn_prefab. */
static bool export_call_has(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    size_t pos = 0;
    while (pos < hay.size()) {
        pos = hay.find(needle, pos);
        if (pos == std::string::npos) return false;
        char left = pos == 0 ? ' ' : hay[pos - 1];
        bool leftOk = !((left >= 'a' && left <= 'z') || (left >= '0' && left <= '9'));
        size_t end = pos + needle.size();
        char right = end == hay.size() ? ' ' : hay[end];
        bool rightOk = !((right >= 'a' && right <= 'z') || (right >= '0' && right <= '9'));
        if (leftOk && rightOk) return true;
        pos += 1;
    }
    return false;
}

static bool export_skip_name(const std::string& name) {
    if (name.empty() || name[0] == '.') return true;
    std::string l = export_norm(name);
    return l == "thumbs.db" || l == "desktop.ini";
}

static void export_add_abs(const fs::path& abs, const std::string& rel) {
    std::error_code ec;
    if (!fs::is_regular_file(abs, ec)) return;
    ExportCat c;
    c.rel = rel;
    for (char& ch : c.rel) if (ch == '\\') ch = '/';
    c.key = export_norm(c.rel);
    if (c.key.empty() || g_xby.count(c.key)) return;
    g_xby[c.key] = (int)g_xcat.size();
    g_xcat.push_back(std::move(c));
}

static void export_add_tree(const fs::path& dir, const std::string& prefix) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code rec;
        if (it->is_regular_file(rec) && !rec && !export_skip_name(it->path().filename().string())) {
            std::string rel = fs::relative(it->path(), dir, rec).generic_string();
            if (!rec && !rel.empty()) export_add_abs(it->path(), prefix + rel);
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
}

static bool export_scannable(const std::string& ext) {
    return ext == ".nxa" || ext == ".scene" || ext == ".prefab" ||
           ext == ".obj" || ext == ".mtl" || ext == ".cfg";
}

static bool export_image_ext(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}

static void export_enqueue_rel(const std::string& rel);

static void export_enqueue_dir_images(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    while (!ec && it != end) {
        std::error_code rec;
        if (it->is_regular_file(rec) && !rec) {
            std::string ext = export_ext(export_norm(it->path().filename().string()));
            if (export_image_ext(ext)) {
                std::string rel = fs::relative(it->path(), fs::path(g.projRoot), rec).generic_string();
                if (!rec && !rel.empty()) export_enqueue_rel(rel);
            }
        }
        it.increment(ec);
        if (ec) ec.clear();
    }
}

static void export_enqueue_model_images(const std::string& objRel) {
    fs::path folder = (fs::path(g.projRoot) / fs::path(objRel)).parent_path();
    export_enqueue_dir_images(folder);
    export_enqueue_dir_images(folder / "textures");
    export_enqueue_dir_images(folder / "texture");
}

static void export_enqueue(int idx);

static std::string export_read(const std::string& rel) {
    fs::path p = fs::path(g.projRoot) / fs::path(rel);
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return "";
    if (n > 2 * 1024 * 1024) n = 2 * 1024 * 1024;
    std::string s((size_t)n, '\0');
    f.seekg(0);
    f.read(&s[0], (std::streamsize)n);
    for (char& c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

static void export_scan(int idx) {
    if (idx < 0 || idx >= (int)g_xcat.size()) return;
    const std::string& key = g_xcat[idx].key;
    std::string ext = export_ext(key);
    if (!export_scannable(ext)) return;
    std::string hay = export_read(g_xcat[idx].rel);
    if (ext == ".nxa") {
        if (export_call_has(hay, "scene_load_by_index") || export_call_has(hay, "scene_load_game_by_index"))
            g_xwantScenes = 1;
        if (export_call_has(hay, "spawn_prefab"))
            g_xwantPrefabs = 1;
    }
    for (const XNeedle& n : g_xneedles) {
        if (n.cat == idx) continue;
        if (export_hay_has(hay, n.text)) export_enqueue(n.cat);
    }
    /* Model pictures are chosen by filename in the model folder. The OBJ
       usually never writes those names down, so a text scan misses them. */
    if (ext == ".obj") export_enqueue_model_images(g_xcat[idx].rel);
}

static void export_enqueue(int idx) {
    if (idx < 0 || idx >= (int)g_xcat.size()) return;
    const std::string& key = g_xcat[idx].key;
    if (!g_exportKeys.insert(key).second) return;
    std::string ext = export_ext(key);
    /* The game runs bin/<name>.dll. The .nxa and .ebp files are the source. */
    if (ext != ".nxa" && ext != ".ebp") g_exportUsed.push_back(g_xcat[idx].rel);
    if (export_scannable(ext)) g_xqueue.push_back(idx);
    if (ext == ".nxa" && key.rfind("scripts/", 0) == 0 && key.size() > 12) {
        std::string dll = "bin/" + key.substr(8);
        dll.replace(dll.size() - 4, 4, ".dll");
        auto it = g_xby.find(dll);
        if (it != g_xby.end()) export_enqueue(it->second);
        else log_push("[export] no compiled dll for " + g_xcat[idx].rel);
    }
}

static void export_enqueue_rel(const std::string& rel) {
    auto it = g_xby.find(export_norm(rel));
    if (it != g_xby.end()) export_enqueue(it->second);
}

static void export_enqueue_prefix(const char* prefix) {
    std::string p = prefix;
    for (int i = 0; i < (int)g_xcat.size(); ++i) {
        if (g_xcat[i].key.rfind(p, 0) == 0) export_enqueue(i);
    }
}

static int export_bucket(const std::string& key) {
    if (key.rfind("scenes/", 0) == 0) return 0;
    if (key.rfind("scripts/", 0) == 0 && export_ext(key) == ".nxa") return 1;
    if (key.rfind("assets/", 0) == 0) return 2;
    if (key.rfind("prefabs/", 0) == 0) return 3;
    return -1;
}

extern "C" int engine_export_used_count() {
    g_xcat.clear();
    g_xby.clear();
    g_xqueue.clear();
    g_xneedles.clear();
    g_exportUsed.clear();
    g_exportKeys.clear();
    g_exportStart = -1;
    g_xwantScenes = 0;
    g_xwantPrefabs = 0;
    if (!g.projLoaded) return 0;

    fs::path root(g.projRoot);
    export_add_tree(root / "scenes", "scenes/");
    export_add_tree(root / "scripts", "scripts/");
    export_add_tree(root / "assets", "assets/");
    export_add_tree(root / "prefabs", "prefabs/");
    export_add_tree(root / "bin", "bin/");
    export_add_abs(root / "editor.cfg", "editor.cfg");
    export_add_abs(root / "splash.cfg", "splash.cfg");

    std::unordered_set<std::string> seenNeedle;
    for (int i = 0; i < (int)g_xcat.size(); ++i) {
        const std::string& key = g_xcat[i].key;
        auto add = [&](const std::string& text) {
            if (text.size() < 5 || text.find('.') == std::string::npos) return;
            std::string id = text + "\n" + std::to_string(i);
            if (!seenNeedle.insert(id).second) return;
            g_xneedles.push_back(XNeedle{ text, i });
        };
        add(key);
        auto slash = key.rfind('/');
        if (slash != std::string::npos) add(key.substr(slash + 1));
        auto first = key.find('/');
        if (first != std::string::npos && first + 1 < key.size()) add(key.substr(first + 1));
    }

    if (!g.projScenes.empty()) {
        g_exportStart = 0;
        if (!g.projStart.empty()) {
            std::string want = export_norm(g.projStart);
            for (int i = 0; i < (int)g.projScenes.size(); ++i) {
                if (export_norm(g.projScenes[i]) == want) { g_exportStart = i; break; }
            }
        }
        export_enqueue_rel(g.projScenes[g_exportStart]);
    }
    export_enqueue_rel("splash.cfg");
    for (const SplashItem& s : g_splash) export_enqueue_rel(s.image);

    bool pulledScenes = false;
    bool pulledPrefabs = false;
    for (;;) {
        size_t q = 0;
        while (q < g_xqueue.size()) {
            int idx = g_xqueue[q++];
            export_scan(idx);
        }
        if (g_xwantScenes && !pulledScenes) {
            pulledScenes = true;
            log_push("[export] a script loads a scene by number, so every scene is included");
            export_enqueue_prefix("scenes/");
            continue;
        }
        if (g_xwantPrefabs && !pulledPrefabs) {
            pulledPrefabs = true;
            log_push("[export] a script spawns a prefab by number, so every prefab is included");
            export_enqueue_prefix("prefabs/");
            continue;
        }
        break;
    }
    export_enqueue_rel("editor.cfg");

    int kept[4] = { 0, 0, 0, 0 };
    int left[4] = { 0, 0, 0, 0 };
    for (const ExportCat& c : g_xcat) {
        int b = export_bucket(c.key);
        if (b < 0) continue;
        if (g_exportKeys.count(c.key)) kept[b] += 1;
        else left[b] += 1;
    }
    std::string startName = "(none)";
    if (g_exportStart >= 0 && g_exportStart < (int)g.projScenes.size()) {
        startName = g.projScenes[g_exportStart];
        for (char& ch : startName) if (ch == '\\') ch = '/';
    }
    log_push("[export] launch scene " + startName +
             ": " + std::to_string(kept[0]) + " scenes, " +
             std::to_string(kept[1]) + " scripts, " +
             std::to_string(kept[2]) + " assets, " +
             std::to_string(kept[3]) + " prefabs");
    int leftN = left[0] + left[1] + left[2] + left[3];
    if (leftN == 0) log_push("[export] nothing unused");
    else {
        log_push("[export] left out " + std::to_string(left[0]) + " scenes, " +
                 std::to_string(left[1]) + " scripts, " +
                 std::to_string(left[2]) + " assets, " +
                 std::to_string(left[3]) + " prefabs");
    }
    return (int)g_exportUsed.size();
}

extern "C" const char* engine_export_used_rel(int i) {
    if (i < 0 || i >= (int)g_exportUsed.size()) return "";
    return g_exportUsed[i].c_str();
}

extern "C" int engine_export_used_has(const char* rel) {
    if (!rel) return 0;
    return g_exportKeys.count(export_norm(rel)) ? 1 : 0;
}

extern "C" int engine_export_start_index() {
    return g_exportStart;
}

/* ----- file-open dialog (Win32 GetOpenFileName) ---------------------------- */

/* Win32 OPENFILENAME wants its filter as "Label\0pattern\0Label2\0pattern2\0\0".
 * We can't smuggle that through Nexa's c_str(), so callers pass it as
 * "Label|pattern|Label2|pattern2" and we convert pipes -> NULs here. */
extern "C" const char* engine_open_file_dialog(const char* title,
                                               const char* filter,
                                               const char* initial_dir) {
    static std::string cached;
    cached.clear();

    std::string f = (filter && *filter) ? filter : "All files|*.*";
    /* convert | to NULs and append double-NUL terminator */
    std::string filterBuf;
    filterBuf.reserve(f.size() + 2);
    for (char c : f) filterBuf.push_back(c == '|' ? '\0' : c);
    filterBuf.push_back('\0');
    filterBuf.push_back('\0');

    char buf[1024] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g.hwnd;
    ofn.lpstrFilter = filterBuf.c_str();
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.lpstrTitle  = (title && *title) ? title : nullptr;
    ofn.lpstrInitialDir = (initial_dir && *initial_dir) ? initial_dir : nullptr;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn) == FALSE) return "";
    cached = buf;
    return cached.c_str();
}

/* ----- folder picker (Vista+ IFileOpenDialog in folder mode) --------------- */

static std::string wide_to_acp(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

/* Returns the chosen folder, or "" if cancelled. */
extern "C" const char* engine_open_folder_dialog(const char* title, const char* initial_dir) {
    static std::string cached;
    cached.clear();
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFileOpenDialog, (void**)&dlg))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (title && *title) {
            std::wstring wt(title, title + strlen(title));
            dlg->SetTitle(wt.c_str());
        }
        if (initial_dir && *initial_dir) {
            int n = MultiByteToWideChar(CP_ACP, 0, initial_dir, -1, nullptr, 0);
            std::wstring wd((size_t)n, L'\0');
            MultiByteToWideChar(CP_ACP, 0, initial_dir, -1, &wd[0], n);
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(wd.c_str(), nullptr, IID_IShellItem, (void**)&start))) {
                dlg->SetFolder(start);
                start->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(g.hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    cached = wide_to_acp(p);
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return cached.c_str();
}

/* ----- clipboard ------------------------------------------------------------- */

/* First line of the clipboard's text, trimmed, with one pair of surrounding
 * quotes removed (Explorer's "Copy as path" adds them). For single-line
 * fields; "" when the clipboard holds no text. */
extern "C" const char* engine_clipboard_line() {
    static std::string cached;
    cached.clear();
    if (!OpenClipboard(g.hwnd)) return "";
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* w = (const wchar_t*)GlobalLock(h);
        if (w) {
            cached = wide_to_acp(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    size_t eol = cached.find_first_of("\r\n");
    if (eol != std::string::npos) cached.resize(eol);
    size_t a = cached.find_first_not_of(" \t");
    size_t b = cached.find_last_not_of(" \t");
    cached = (a == std::string::npos) ? std::string() : cached.substr(a, b - a + 1);
    if (cached.size() >= 2 && cached.front() == '"' && cached.back() == '"')
        cached = cached.substr(1, cached.size() - 2);
    return cached.c_str();
}

extern "C" void engine_clipboard_set(const char* text) {
    if (!text) text = "";
    int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (n <= 0) return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!mem) return;
    wchar_t* w = (wchar_t*)GlobalLock(mem);
    if (!w) { GlobalFree(mem); return; }
    MultiByteToWideChar(CP_ACP, 0, text, -1, w, n);
    GlobalUnlock(mem);
    if (!OpenClipboard(g.hwnd)) { GlobalFree(mem); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    CloseClipboard();
}

extern "C" int engine_project_recent_count() { return (int)g.projRecent.size(); }
extern "C" const char* engine_project_recent(int i) {
    if (i < 0 || i >= (int)g.projRecent.size()) return "";
    return g.projRecent[i].c_str();
}
extern "C" void engine_project_recent_set_file(const char* path) {
    if (path) g.projRecentFile = path;
}
extern "C" void engine_project_recent_load() {
    g.projRecent.clear();
    if (g.projRecentFile.empty()) return;
    std::ifstream f(g.projRecentFile);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) g.projRecent.push_back(line);
        if (g.projRecent.size() >= 16) break;
    }
}
extern "C" void engine_project_recent_save() {
    if (g.projRecentFile.empty()) return;
    std::ofstream f(g.projRecentFile, std::ios::binary);
    if (!f) return;
    for (auto& p : g.projRecent) f << p << "\n";
}
extern "C" void engine_project_recent_push(const char* path) {
    if (!path) return;
    std::string p = path;
    g.projRecent.erase(std::remove(g.projRecent.begin(), g.projRecent.end(), p),
                       g.projRecent.end());
    g.projRecent.insert(g.projRecent.begin(), p);
    if (g.projRecent.size() > 16) g.projRecent.resize(16);
    engine_project_recent_save();
}
extern "C" void engine_project_recent_remove(const char* path) {
    if (!path) return;
    std::string p = path;
    g.projRecent.erase(std::remove(g.projRecent.begin(), g.projRecent.end(), p),
                       g.projRecent.end());
    engine_project_recent_save();
}
extern "C" void engine_project_recent_clear() {
    g.projRecent.clear();
    engine_project_recent_save();
}

/* ============================================================ */
/*  Camera view                                                 */
/* ============================================================ */

extern "C" void engine_view_set(float cam_x, float cam_y, float zoom,
                                int view_x, int view_y, int view_w, int view_h) {
    g.viewCamX = cam_x;
    g.viewCamY = cam_y;
    g.viewZoom = (zoom < 0.05f) ? 0.05f : zoom;
    g.viewX = view_x;
    g.viewY = view_y;
    g.viewW = view_w;
    g.viewH = view_h;
}
extern "C" float engine_view_cam_x() { return g.viewCamX; }
extern "C" float engine_view_cam_y() { return g.viewCamY; }
extern "C" float engine_view_zoom()  { return g.viewZoom; }
extern "C" int   engine_view_x() { return g.viewX; }
extern "C" int   engine_view_y() { return g.viewY; }
extern "C" int   engine_view_w() { return g.viewW; }
extern "C" int   engine_view_h() { return g.viewH; }

extern "C" int engine_screen_to_world_x(int sx) {
    return (int)std::floor((sx - g.viewX) / g.viewZoom + g.viewCamX);
}
extern "C" int engine_screen_to_world_y(int sy) {
    return (int)std::floor((sy - g.viewY) / g.viewZoom + g.viewCamY);
}
extern "C" int engine_world_to_screen_x(int wx) {
    return g.viewX + (int)std::floor((wx - g.viewCamX) * g.viewZoom);
}
extern "C" int engine_world_to_screen_y(int wy) {
    return g.viewY + (int)std::floor((wy - g.viewCamY) * g.viewZoom);
}
extern "C" int engine_world_to_screen_len(int wl) {
    return (int)std::floor(wl * g.viewZoom);
}

/* ============================================================ */
/*  Tilemap                                                     */
/* ============================================================ */

static int tile_in_bounds(int tx, int ty) {
    return tx >= 0 && ty >= 0 && tx < g_tiles.w && ty < g_tiles.h;
}

static uint8_t tile_get(int tx, int ty) {
    if (!tile_in_bounds(tx, ty)) return 0;
    return g_tiles.cells[(size_t)ty * (size_t)g_tiles.w + (size_t)tx];
}

static void tile_set(int tx, int ty, int id) {
    if (!tile_in_bounds(tx, ty)) return;
    if (id < 0) id = 0;
    if (id > 255) id = 255;
    g_tiles.cells[(size_t)ty * (size_t)g_tiles.w + (size_t)tx] = (uint8_t)id;
}

static int tile_is_solid(int tx, int ty) {
    uint8_t id = tile_get(tx, ty);
    return g_tiles.defs[id].solid ? 1 : 0;
}

static uint32_t noise_hash(int x, int y, int seed) {
    /* All three products overflow int for any realistic coordinate, which is
       signed overflow, which is undefined. Left signed, -O2 is entitled to
       assume it cannot happen and the results stop obeying arithmetic (a
       following `% 1000` was being folded away at some call sites). Do the
       mixing in unsigned, where wrapping is defined. */
    uint32_t n = (uint32_t)x * 374761393u
               + (uint32_t)y * 668265263u
               + (uint32_t)seed * 1442695041u;
    n = (n ^ (n >> 13)) * 1274126177u;
    n ^= n >> 16;
    return n;
}

extern "C" int engine_noise1(int x, int seed) {
    int x0 = x >> 4;
    int t = x & 15;
    int a = (int)(noise_hash(x0, 0, seed) & 1023);
    int b = (int)(noise_hash(x0 + 1, 0, seed) & 1023);
    return a + (b - a) * t / 16;
}

extern "C" int engine_noise2(int x, int y, int seed) {
    return (int)(noise_hash(x, y, seed) & 1023);
}

extern "C" int engine_tilemap_create(int w, int h, int tile_size) {
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    if (tile_size < 4) tile_size = 4;
    if (tile_size > 128) tile_size = 128;
    g_tiles.w = w;
    g_tiles.h = h;
    g_tiles.tile = tile_size;
    g_tiles.cells.assign((size_t)w * (size_t)h, 0);
    g_tiles.cursorTx = -1;
    g_tiles.cursorTy = -1;
    if (!g_tiles.defs[0].solid && g_tiles.defs[0].r == 0 && g_tiles.defs[0].sprite.empty()) {
        g_tiles.defs[0] = TileDef{};
    }
    return 1;
}

extern "C" void engine_tilemap_destroy() {
    g_tiles.w = 0;
    g_tiles.h = 0;
    g_tiles.cells.clear();
    g_tiles.cursorTx = -1;
    g_tiles.cursorTy = -1;
}

extern "C" int engine_tilemap_active() {
    if (g_tilemap_draw_hidden) return 0;
    return (g_tiles.w > 0 && g_tiles.h > 0) ? 1 : 0;
}
extern "C" int engine_tilemap_width() { return g_tiles.w; }
extern "C" int engine_tilemap_height() { return g_tiles.h; }
extern "C" int engine_tilemap_tile_size() { return g_tiles.tile; }

extern "C" int engine_tilemap_get(int tx, int ty) { return (int)tile_get(tx, ty); }
extern "C" void engine_tilemap_set(int tx, int ty, int id) { tile_set(tx, ty, id); }

extern "C" void engine_tilemap_fill(int tx, int ty, int tw, int th, int id) {
    if (id < 0) id = 0;
    if (id > 255) id = 255;
    int x0 = std::max(0, tx);
    int y0 = std::max(0, ty);
    int x1 = std::min(g_tiles.w, tx + tw);
    int y1 = std::min(g_tiles.h, ty + th);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            g_tiles.cells[(size_t)y * (size_t)g_tiles.w + (size_t)x] = (uint8_t)id;
        }
    }
}

extern "C" void engine_tilemap_fill_column(int tx, int y0, int y1, int id) {
    if (tx < 0 || tx >= g_tiles.w) return;
    if (id < 0) id = 0;
    if (id > 255) id = 255;
    if (y0 < 0) y0 = 0;
    if (y1 > g_tiles.h) y1 = g_tiles.h;
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y < y1; ++y) {
        g_tiles.cells[(size_t)y * (size_t)g_tiles.w + (size_t)tx] = (uint8_t)id;
    }
}

extern "C" void engine_tilemap_define(int id, int r, int g_, int b, int solid, const char* sprite) {
    if (id < 0 || id > 255) return;
    TileDef& d = g_tiles.defs[id];
    d.r = r; d.g = g_; d.b = b;
    d.r2 = std::max(0, r - 28);
    d.g2 = std::max(0, g_ - 28);
    d.b2 = std::max(0, b - 28);
    d.solid = solid ? 1 : 0;
    d.sprite = sprite ? sprite : "";
    d.spriteId = 0;
    d.variantIds.clear();
    d.sheetId = 0;
    d.sheetFrames.clear();
    for (int m = 0; m < 16; ++m) d.autoFrames[m].clear();
    d.autotile = 0;
    d.autoOverride = 0;
    if (!d.sprite.empty()) {
        d.spriteId = engine_image_load_cached(d.sprite.c_str());
        if (d.spriteId != 0) d.variantIds.push_back(d.spriteId);
    }
}

extern "C" void engine_tilemap_add_variant(int id, const char* sprite) {
    if (id < 0 || id > 255 || !sprite || !*sprite) return;
    TileDef& d = g_tiles.defs[id];
    int sid = engine_image_load_cached(sprite);
    if (sid == 0) return;
    if (d.spriteId == 0) {
        d.sprite = sprite;
        d.spriteId = sid;
    }
    for (int existing : d.variantIds) {
        if (existing == sid) return;
    }
    d.variantIds.push_back(sid);
}

extern "C" void engine_tilemap_sheet(int id, const char* path, int cell, int stride) {
    if (id < 0 || id > 255 || !path || !*path) return;
    TileDef& d = g_tiles.defs[id];
    if (cell < 1) cell = 16;
    if (stride < cell) stride = cell;
    d.sheetCell = cell;
    d.sheetStride = stride;
    d.sheetFrames.clear();
    d.sheetId = engine_image_load_cached(path);
}

extern "C" void engine_tilemap_sheet_cell(int id, int col, int row) {
    if (id < 0 || id > 255) return;
    TileDef& d = g_tiles.defs[id];
    if (d.sheetId == 0) return;
    if (col < 0 || row < 0) return;
    TileSheetFrame f;
    f.sx = col * d.sheetStride;
    f.sy = row * d.sheetStride;
    f.sw = d.sheetCell;
    f.sh = d.sheetCell;
    f.tilesX = 1;
    f.tilesY = 1;
    f.stride = d.sheetStride;
    d.sheetFrames.push_back(f);
}

extern "C" void engine_tilemap_sheet_block(int id, int col, int row) {
    if (id < 0 || id > 255) return;
    TileDef& d = g_tiles.defs[id];
    if (d.sheetId == 0) return;
    if (col < 0 || row < 0) return;
    TileSheetFrame f;
    f.sx = col * d.sheetStride;
    f.sy = row * d.sheetStride;
    f.sw = d.sheetCell;
    f.sh = d.sheetCell;
    f.tilesX = 2;
    f.tilesY = 2;
    f.stride = d.sheetStride;
    d.sheetFrames.push_back(f);
}

/* Load a Terraria-style autotile sheet: a grid of `cell`-pixel frames on a
   `stride` pitch (16 on 18, i.e. a 2px gutter, for Terraria's own sheets),
   where each frame is a complete tile drawn for one particular arrangement of
   neighbours.

   Which arrangement is read off the art rather than taken from a table. Such
   sheets mark a block's exposed sides with a dark outline and round the
   corners off into transparency, so an edge is exposed when at least half of
   its pixels are transparent or outline-dark. That is a property of what the
   frame depicts; the row/column it happens to sit in is a property of one
   sheet, and hardcoding that is what produced vertical stripes and blobs when
   this was tried before - the numbers were a guess at someone else's layout.
   Reading the art also means a replacement sheet just works. */
extern "C" void engine_tilemap_sheet_autotile(int id, const char* path,
                                              int cell, int stride,
                                              int use_cols, int use_rows) {
    if (id < 0 || id > 255 || !path || !*path) return;
    TileDef& d = g_tiles.defs[id];
    if (cell < 1) cell = 16;
    if (stride < cell) stride = cell;
    d.sheetCell = cell;
    d.sheetStride = stride;
    d.sheetFrames.clear();
    for (int m = 0; m < 16; ++m) d.autoFrames[m].clear();
    d.autotile = 0;
    d.autoOverride = 0;

    d.sheetId = engine_image_load_cached(path);
    if (d.sheetId == 0) return;
    auto it = g.images.find(d.sheetId);
    if (it == g.images.end() || !it->second.bmp) return;
    Gdiplus::Bitmap* bmp = it->second.bmp;
    int iw = (int)bmp->GetWidth();
    int ih = (int)bmp->GetHeight();
    if (iw < cell || ih < cell) return;

    Gdiplus::Rect rc(0, 0, iw, ih);
    Gdiplus::BitmapData bd;
    if (bmp->LockBits(&rc, Gdiplus::ImageLockModeRead,
                      PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return;
    const uint8_t* base = (const uint8_t*)bd.Scan0;

    /* 32bppARGB is BGRA in memory, so blue is the low byte. */
    auto px = [&](int x, int y) -> const uint8_t* {
        return base + (ptrdiff_t)y * bd.Stride + (ptrdiff_t)x * 4;
    };
    auto edge_exposed = [&](int x0, int y0, int side) {
        int hits = 0;
        for (int k = 0; k < cell; ++k) {
            int x = (side == 2) ? x0 : (side == 3) ? x0 + cell - 1 : x0 + k;
            int y = (side == 0) ? y0 : (side == 1) ? y0 + cell - 1 : y0 + k;
            const uint8_t* p = px(x, y);
            if (p[3] < 128) { ++hits; continue; }
            if ((p[2] * 2 + p[1] * 5 + p[0]) / 8 < 55) ++hits;
        }
        return hits * 2 >= cell;
    };

    int cols = (iw + stride - cell) / stride;
    int rows = (ih + stride - cell) / stride;
    /* A sheet often holds several complete sets side by side - one per blend
       style. Pooling them all puts unrelated art in the same bucket, so
       neighbouring cells get frames drawn in different styles and the ground
       looks like a patchwork. use_cols/use_rows bound it to one set. */
    if (use_cols > 0 && use_cols < cols) cols = use_cols;
    if (use_rows > 0 && use_rows < rows) rows = use_rows;
    int total = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            int x0 = c * stride;
            int y0 = r * stride;
            if (x0 + cell > iw || y0 + cell > ih) continue;

            int opaque_px = 0;
            for (int j = 0; j < cell; ++j) {
                for (int i = 0; i < cell; ++i) {
                    if (px(x0 + i, y0 + j)[3] >= 128) ++opaque_px;
                }
            }
            if (opaque_px == 0) continue;     /* unused slot in the sheet */

            int m = 0;
            if (edge_exposed(x0, y0, 0)) m |= 1;
            if (edge_exposed(x0, y0, 1)) m |= 2;
            if (edge_exposed(x0, y0, 2)) m |= 4;
            if (edge_exposed(x0, y0, 3)) m |= 8;

            TileSheetFrame f;
            f.sx = x0;
            f.sy = y0;
            f.sw = cell;
            f.sh = cell;
            f.tilesX = 1;
            f.tilesY = 1;
            f.stride = stride;
            f.opaque = (opaque_px == cell * cell) ? 1 : 0;
            d.autoFrames[m].push_back(f);
            ++total;
        }
    }
    bmp->UnlockBits(&bd);
    if (total == 0) return;

    /* Any arrangement the sheet has no art for borrows from the closest one
       it does, so a cell never renders as a hole. Which buckets were real is
       snapshotted first, or a filled-in bucket could be copied onward. */
    bool have[16];
    for (int m = 0; m < 16; ++m) have[m] = !d.autoFrames[m].empty();
    for (int m = 0; m < 16; ++m) {
        if (have[m]) continue;
        int best = -1;
        int bestDist = 99;
        for (int n = 0; n < 16; ++n) {
            if (!have[n]) continue;
            int dist = 0;
            for (int bit = 0; bit < 4; ++bit) {
                if (((m >> bit) & 1) != ((n >> bit) & 1)) ++dist;
            }
            if (dist < bestDist) { bestDist = dist; best = n; }
        }
        if (best >= 0) d.autoFrames[m] = d.autoFrames[best];
    }
    d.autotile = 1;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "[tilemap] autotile id=%d %dx%d cells, %d frames", id, cols, rows, total);
    engine_log(msg);
}

/* Pin a specific sheet frame to a specific neighbour arrangement. The first
   call for a mask replaces whatever engine_tilemap_sheet_autotile derived for
   it; later calls add variants.

   Needed for the fully-enclosed case. Every other arrangement is identifiable
   from the art because its open sides carry an outline, but a surrounded frame
   and a "covered by a neighbouring block type" blend frame both have four
   clean edges and cannot be told apart that way. Guessing wrong there is
   costly: enclosed cells are most of what is on screen underground. */
extern "C" void engine_tilemap_autotile_frame(int id, int mask, int col, int row) {
    if (id < 0 || id > 255 || mask < 0 || mask > 15) return;
    if (col < 0 || row < 0) return;
    TileDef& d = g_tiles.defs[id];
    if (d.sheetId == 0) return;
    auto it = g.images.find(d.sheetId);
    if (it == g.images.end() || !it->second.bmp) return;
    Gdiplus::Bitmap* bmp = it->second.bmp;
    int cell = d.sheetCell;
    int stride = d.sheetStride;
    int x0 = col * stride;
    int y0 = row * stride;
    if (x0 + cell > (int)bmp->GetWidth() || y0 + cell > (int)bmp->GetHeight()) return;

    if ((d.autoOverride & (1 << mask)) == 0) {
        d.autoFrames[mask].clear();
        d.autoOverride |= (1 << mask);
    }

    /* Opacity decides whether the frame can skip the transparency mask. */
    int opaque_px = 0;
    Gdiplus::Rect rc(x0, y0, cell, cell);
    Gdiplus::BitmapData bd;
    if (bmp->LockBits(&rc, Gdiplus::ImageLockModeRead,
                      PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        for (int j = 0; j < cell; ++j) {
            const uint8_t* p = (const uint8_t*)bd.Scan0 + (ptrdiff_t)j * bd.Stride;
            for (int i = 0; i < cell; ++i) {
                if (p[i * 4 + 3] >= 128) ++opaque_px;
            }
        }
        bmp->UnlockBits(&bd);
    }

    TileSheetFrame f;
    f.sx = x0;
    f.sy = y0;
    f.sw = cell;
    f.sh = cell;
    f.tilesX = 1;
    f.tilesY = 1;
    f.stride = stride;
    f.opaque = (opaque_px == cell * cell) ? 1 : 0;
    d.autoFrames[mask].push_back(f);
    d.autotile = 1;
}

extern "C" void engine_tilemap_backdrop_clear() { g_backdrop.clear(); }

/* Add a parallax layer. par is per-mille of camera movement: 0 pins the layer
   to the screen, 1000 moves it with the world, so distant scenery wants a
   small number. fill_view stretches the layer over the whole view in screen
   space (for a sky); otherwise its bottom edge sits on world Y anchor_y and it
   is drawn at scale per-mille of native size, in world pixels. */
extern "C" void engine_tilemap_backdrop_add(int image_id, int par,
                                            int anchor_y, int scale,
                                            int fill_view) {
    if (image_id == 0) return;
    if (g_backdrop.size() >= 8) return;
    BackdropLayer L;
    L.imageId = image_id;
    L.par = par;
    L.anchorY = anchor_y;
    L.scale = (scale > 0) ? scale : 1000;
    L.fillView = fill_view ? 1 : 0;
    g_backdrop.push_back(L);
}

extern "C" int engine_tilemap_solid(int tx, int ty) { return tile_is_solid(tx, ty); }
extern "C" int engine_tilemap_solid_at(int px, int py) {
    if (g_tiles.tile <= 0) return 0;
    return tile_is_solid(px / g_tiles.tile, py / g_tiles.tile);
}

static int rect_hits_solid(int x, int y, int w, int h) {
    if (g_tiles.tile <= 0 || w <= 0 || h <= 0) return 0;
    int x0 = x / g_tiles.tile;
    int y0 = y / g_tiles.tile;
    int x1 = (x + w - 1) / g_tiles.tile;
    int y1 = (y + h - 1) / g_tiles.tile;
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            if (tile_is_solid(tx, ty)) return 1;
        }
    }
    return 0;
}

extern "C" int engine_tilemap_collide_rect(int x, int y, int w, int h) {
    return rect_hits_solid(x, y, w, h);
}

static int resolve_axis(int* pos, int size, int delta, int other, int other_size, int horizontal) {
    int hit = 0;
    if (delta == 0) return 0;
    int next = *pos + delta;
    if (!rect_hits_solid(horizontal ? next : other,
                         horizontal ? other : next,
                         horizontal ? size : other_size,
                         horizontal ? other_size : size)) {
        *pos = next;
        return 0;
    }
    int step = (delta > 0) ? 1 : -1;
    int remaining = (delta > 0) ? delta : -delta;
    while (remaining > 0) {
        int trial = *pos + step;
        if (rect_hits_solid(horizontal ? trial : other,
                            horizontal ? other : trial,
                            horizontal ? size : other_size,
                            horizontal ? other_size : size)) {
            hit = 1;
            break;
        }
        *pos = trial;
        --remaining;
    }
    return hit;
}

extern "C" void engine_tilemap_move(int x, int y, int w, int h, int dx, int dy) {
    g_tiles.lastHitX = 0;
    g_tiles.lastHitY = 0;
    g_tiles.lastGrounded = 0;
    if (g_tiles.w <= 0) {
        g_tiles.lastX = x + dx;
        g_tiles.lastY = y + dy;
        return;
    }
    int nx = x;
    int ny = y;
    g_tiles.lastHitX = resolve_axis(&nx, w, dx, ny, h, 1);
    g_tiles.lastHitY = resolve_axis(&ny, h, dy, nx, w, 0);
    if (dy >= 0 && rect_hits_solid(nx, ny + 1, w, h)) g_tiles.lastGrounded = 1;
    g_tiles.lastX = nx;
    g_tiles.lastY = ny;
}

extern "C" int engine_tilemap_last_x() { return g_tiles.lastX; }
extern "C" int engine_tilemap_last_y() { return g_tiles.lastY; }
extern "C" int engine_tilemap_last_hit_x() { return g_tiles.lastHitX; }
extern "C" int engine_tilemap_last_hit_y() { return g_tiles.lastHitY; }
extern "C" int engine_tilemap_last_grounded() { return g_tiles.lastGrounded; }

extern "C" void engine_tilemap_move_entity(int slot, int dx, int dy) {
    if (!active_slot(slot)) return;
    Entity& e = g.entities[slot];
    engine_tilemap_move(e.x, e.y, e.w, e.h, dx, dy);
    e.x = g_tiles.lastX;
    e.y = g_tiles.lastY;
}

extern "C" int engine_tilemap_world_to_tile_x(int px) {
    if (g_tiles.tile <= 0) return 0;
    return px / g_tiles.tile;
}
extern "C" int engine_tilemap_world_to_tile_y(int py) {
    if (g_tiles.tile <= 0) return 0;
    return py / g_tiles.tile;
}
extern "C" int engine_tilemap_tile_to_world_x(int tx) { return tx * g_tiles.tile; }
extern "C" int engine_tilemap_tile_to_world_y(int ty) { return ty * g_tiles.tile; }

extern "C" int engine_tilemap_surface_y(int tx) {
    if (!tile_in_bounds(tx, 0)) return g_tiles.h;
    for (int y = 0; y < g_tiles.h; ++y) {
        if (tile_is_solid(tx, y)) return y;
    }
    return g_tiles.h;
}

extern "C" void engine_tilemap_set_sky(int r, int g_, int b) {
    g_tiles.skyR = r;
    g_tiles.skyG = g_;
    g_tiles.skyB = b;
}

extern "C" void engine_tilemap_set_cursor(int tx, int ty) {
    g_tiles.cursorTx = tx;
    g_tiles.cursorTy = ty;
}

extern "C" void engine_tilemap_noise_replace(int from_id, int to_id,
                                             int y0, int y1,
                                             int threshold, int seed, int freq) {
    if (g_tiles.w <= 0) return;
    if (freq < 1) freq = 1;
    if (y0 < 0) y0 = 0;
    if (y1 > g_tiles.h) y1 = g_tiles.h;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < g_tiles.w; ++x) {
            size_t i = (size_t)y * (size_t)g_tiles.w + (size_t)x;
            if (g_tiles.cells[i] != (uint8_t)from_id) continue;
            int n = engine_noise2(x / freq, y / freq, seed);
            int n2 = engine_noise2(x / (freq + 2), y / (freq + 1), seed + 17);
            if (((n + n2) / 2) > threshold) g_tiles.cells[i] = (uint8_t)to_id;
        }
    }
}

static void tile_fill_px(int x, int y, int w, int h, int r, int g_, int b) {
    if (!g.backDC || w <= 0 || h <= 0) return;
    RECT rc{ x, y, x + w, y + h };
    FillRect(g.backDC, &rc, fill_brush(r, g_, b));
}

/* Whether a neighbour covers an edge for autotiling purposes. Out of bounds
   counts as covered, so the map border is not drawn as a cliff face, and
   anything non-solid - air, water, foliage - counts as open, which is what
   puts a grass crown on the face in the first place. */
static int tile_merges(int tx, int ty) {
    if (!tile_in_bounds(tx, ty)) return 1;
    uint8_t id = tile_get(tx, ty);
    if (id == 0) return 0;
    return g_tiles.defs[id].solid ? 1 : 0;
}

/* StretchBlt a source rect that may have transparency onto the back buffer.
   The mask pair relies on the back DC's default mono colours (a set bit maps
   to white, a clear bit to black), so callers set those once per frame.
     AND with the mask   - keeps the background where the source is see-through,
                           clears it to 0 where the source is opaque;
     OR with the colour  - adds the source, whose transparent pixels are black
                           and so leave the kept background alone. */
static void image_stretch_blit(Image& img, int dx, int dy, int dw, int dh,
                               int sx, int sy, int sw, int sh,
                               int src_opaque = 0) {
    image_ensure_dib(img);
    if (!img.hdc) return;
    /* At zoom 1 a tile is drawn at its source size, which is the common case
       now, and BitBlt skips StretchBlt's scaling setup. With thousands of tile
       blits per frame that setup is most of the cost. */
    bool same = (dw == sw && dh == sh);
    if (src_opaque) {
        if (same) BitBlt(g.backDC, dx, dy, dw, dh, img.hdc, sx, sy, SRCCOPY);
        else StretchBlt(g.backDC, dx, dy, dw, dh, img.hdc, sx, sy, sw, sh, SRCCOPY);
        return;
    }
    image_ensure_mask(img);
    if (img.hasAlpha && img.maskDC) {
        if (same) {
            BitBlt(g.backDC, dx, dy, dw, dh, img.maskDC, sx, sy, SRCAND);
            BitBlt(g.backDC, dx, dy, dw, dh, img.hdc, sx, sy, SRCPAINT);
        } else {
            StretchBlt(g.backDC, dx, dy, dw, dh, img.maskDC, sx, sy, sw, sh, SRCAND);
            StretchBlt(g.backDC, dx, dy, dw, dh, img.hdc, sx, sy, sw, sh, SRCPAINT);
        }
        return;
    }
    if (same) BitBlt(g.backDC, dx, dy, dw, dh, img.hdc, sx, sy, SRCCOPY);
    else StretchBlt(g.backDC, dx, dy, dw, dh, img.hdc, sx, sy, sw, sh, SRCCOPY);
}

/* Draw the backdrop: each layer repeated horizontally to cover the view,
   offset by the camera scaled by its parallax factor.

   Layers go through image_stretch_blit rather than engine_image_blit_pan_nn
   because that one is SRCCOPY only, and three of the four layers are mostly
   transparent - clouds are 98% clear. The mask path handles that. */
static void backdrop_draw() {
    if (g_backdrop.empty() || !g.backDC) return;
    if (g.viewW <= 0 || g.viewH <= 0) return;

    for (size_t i = 0; i < g_backdrop.size(); ++i) {
        const BackdropLayer& L = g_backdrop[i];
        auto it = g.images.find(L.imageId);
        if (it == g.images.end()) continue;
        Image& img = it->second;
        image_ensure_dib(img);
        if (img.w <= 0 || img.h <= 0) continue;

        int dw, dh, dy;
        double scroll;
        if (L.fillView) {
            /* Screen space: fills the view regardless of zoom, so the sky does
               not change size when the player zooms. */
            double sc = (double)g.viewH / (double)img.h;
            dh = g.viewH;
            dy = g.viewY;
            dw = (int)(img.w * sc + 0.5);
            scroll = g.viewCamX * g.viewZoom * (L.par / 1000.0);
        } else {
            /* World space: scales with zoom and sits at a fixed world height,
               so it behaves like real distant scenery. */
            double sc = (L.scale / 1000.0) * g.viewZoom;
            dh = (int)(img.h * sc + 0.5);
            dw = (int)(img.w * sc + 0.5);
            dy = engine_world_to_screen_y(L.anchorY) - dh;
            scroll = g.viewCamX * g.viewZoom * (L.par / 1000.0);
        }
        if (dw < 1 || dh < 1) continue;
        /* skip layers entirely off the top or bottom of the view */
        if (dy + dh <= g.viewY || dy >= g.viewY + g.viewH) continue;

        double m = fmod(scroll, (double)dw);
        if (m < 0.0) m += dw;
        int x = g.viewX - (int)m;
        int right = g.viewX + g.viewW;
        int guard = 0;
        while (x < right && guard < 64) {
            image_stretch_blit(img, x, dy, dw, dh, 0, 0, img.w, img.h);
            x += dw;
            ++guard;
        }
    }
}

static void tile_blit_sheet_frame(Image& img, const TileSheetFrame& cell,
                                  int dx, int dy, int dw, int dh,
                                  Gdiplus::Graphics** gfx) {
    int nx = cell.tilesX > 0 ? cell.tilesX : 1;
    int ny = cell.tilesY > 0 ? cell.tilesY : 1;
    int stride = cell.stride > 0 ? cell.stride : cell.sw;
    image_ensure_dib(img);
    for (int r = 0; r < ny; ++r) {
        int y0 = dy + r * dh / ny;
        int y1 = dy + (r + 1) * dh / ny;
        int qh = y1 - y0;
        if (qh < 1) qh = 1;
        for (int c = 0; c < nx; ++c) {
            int x0 = dx + c * dw / nx;
            int x1 = dx + (c + 1) * dw / nx;
            int qw = x1 - x0;
            if (qw < 1) qw = 1;
            int srcx = cell.sx + c * stride;
            int srcy = cell.sy + r * stride;
            if (img.hdc) {
                image_stretch_blit(img, x0, y0, qw, qh,
                                   srcx, srcy, cell.sw, cell.sh);
                continue;
            }
            if (!img.bmp) continue;
            if (!*gfx) {
                *gfx = new Gdiplus::Graphics(g.backDC);
                (*gfx)->SetInterpolationMode(
                    Gdiplus::InterpolationModeNearestNeighbor);
                (*gfx)->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            }
            (*gfx)->DrawImage(img.bmp,
                              Gdiplus::Rect(x0, y0, qw, qh),
                              srcx, srcy, cell.sw, cell.sh,
                              Gdiplus::UnitPixel);
        }
    }
}

extern "C" void engine_tilemap_draw(int fill_sky) {
    if (g_tilemap_draw_hidden) return;
    if (g_tiles.w <= 0 || g_tiles.h <= 0 || g_tiles.tile <= 0) return;
    if (g.viewW <= 0 || g.viewH <= 0) return;

    if (fill_sky) {
        tile_fill_px(g.viewX, g.viewY, g.viewW, g.viewH, g_tiles.skyR, g_tiles.skyG, g_tiles.skyB);
    }

    int ts = g_tiles.tile;
    int wx0 = engine_screen_to_world_x(g.viewX);
    int wy0 = engine_screen_to_world_y(g.viewY);
    int wx1 = engine_screen_to_world_x(g.viewX + g.viewW);
    int wy1 = engine_screen_to_world_y(g.viewY + g.viewH);
    int tx0 = std::max(0, wx0 / ts - 1);
    int ty0 = std::max(0, wy0 / ts - 1);
    int tx1 = std::min(g_tiles.w - 1, wx1 / ts + 1);
    int ty1 = std::min(g_tiles.h - 1, wy1 / ts + 1);

    HBRUSH brushes[256][2]{};
    bool brush_ready[256] = {};
    /* Atlas cells and whole-tile sprites go through the cached DIB with
       StretchBlt. GDI+ DrawImage per grass cell was loading four PNGs and
       decoding a blit through the software rasterizer every tile. */
    SetStretchBltMode(g.backDC, COLORONCOLOR);
    /* image_stretch_blit's mask pass relies on a set bit mapping to white and
       a clear bit to black, which is what these two give it. */
    SetBkColor(g.backDC, RGB(255, 255, 255));
    SetTextColor(g.backDC, RGB(0, 0, 0));
    backdrop_draw();
    Gdiplus::Graphics* gfx = nullptr;

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            uint8_t id = tile_get(tx, ty);
            if (id == 0) continue;
            const TileDef& d = g_tiles.defs[id];
            int sx = engine_world_to_screen_x(tx * ts);
            int sy = engine_world_to_screen_y(ty * ts);
            int sw = engine_world_to_screen_len(ts);
            int sh = engine_world_to_screen_len(ts);
            if (sw < 1) sw = 1;
            if (sh < 1) sh = 1;

            if (d.autotile && d.sheetId != 0) {
                auto it = g.images.find(d.sheetId);
                if (it != g.images.end() &&
                    (it->second.hdc || it->second.bmp)) {
                    int m = 0;
                    if (!tile_merges(tx, ty - 1)) m |= 1;
                    if (!tile_merges(tx, ty + 1)) m |= 2;
                    if (!tile_merges(tx - 1, ty)) m |= 4;
                    if (!tile_merges(tx + 1, ty)) m |= 8;
                    const std::vector<TileSheetFrame>& bucket = d.autoFrames[m];
                    if (!bucket.empty()) {
                        const TileSheetFrame& f = bucket.size() == 1
                            ? bucket[0]
                            : bucket[noise_hash(tx, ty, 0x5EED)
                                     % (uint32_t)bucket.size()];
                        image_stretch_blit(it->second, sx, sy, sw, sh,
                                           f.sx, f.sy, f.sw, f.sh, f.opaque);
                        continue;
                    }
                }
            }

            if (d.sheetId != 0 && !d.sheetFrames.empty()) {
                const TileSheetFrame& cell = d.sheetFrames.size() == 1
                    ? d.sheetFrames[0]
                    : d.sheetFrames[noise_hash(tx, ty, 0x5EED)
                                    % (uint32_t)d.sheetFrames.size()];
                auto it = g.images.find(d.sheetId);
                if (it != g.images.end() &&
                    (it->second.hdc || it->second.bmp)) {
                    tile_blit_sheet_frame(it->second, cell, sx, sy, sw, sh, &gfx);
                    continue;
                }
            }

            if (d.spriteId != 0) {
                int sid = d.spriteId;
                if (d.variantIds.size() > 1) {
                    /* stable per cell, so a tile does not shimmer as the
                       camera moves */
                    sid = d.variantIds[noise_hash(tx, ty, 0x5EED)
                                       % (uint32_t)d.variantIds.size()];
                }
                auto it = g.images.find(sid);
                if (it != g.images.end()) {
                    Image& img = it->second;
                    image_ensure_dib(img);
                    if (img.hdc) {
                        StretchBlt(g.backDC, sx, sy, sw, sh,
                                   img.hdc, 0, 0, img.w, img.h, SRCCOPY);
                        continue;
                    }
                    if (img.bmp) {
                        if (!gfx) {
                            gfx = new Gdiplus::Graphics(g.backDC);
                            gfx->SetInterpolationMode(
                                Gdiplus::InterpolationModeNearestNeighbor);
                            gfx->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                        }
                        gfx->DrawImage(img.bmp, sx, sy, sw, sh);
                        continue;
                    }
                }
                /* sprite is gone (image cache cleared under us): fall through
                   to the flat colour rather than leaving a hole in the world */
            }

            if (!brush_ready[id]) {
                brushes[id][0] = CreateSolidBrush(RGB(d.r, d.g, d.b));
                brushes[id][1] = CreateSolidBrush(RGB(d.r2, d.g2, d.b2));
                brush_ready[id] = true;
            }
            int shade = (tx + ty) & 1;
            RECT rc{ sx, sy, sx + sw, sy + sh };
            FillRect(g.backDC, &rc, brushes[id][shade]);
            if (sh > 4) {
                int hi = std::min(2, sh / 5);
                tile_fill_px(sx, sy, sw, hi,
                             std::min(255, d.r + 18),
                             std::min(255, d.g + 18),
                             std::min(255, d.b + 18));
            }
        }
    }

    if (gfx) delete gfx;

    for (int i = 0; i < 256; ++i) {
        if (!brush_ready[i]) continue;
        if (brushes[i][0]) DeleteObject(brushes[i][0]);
        if (brushes[i][1]) DeleteObject(brushes[i][1]);
    }

    if (g_tiles.cursorTx >= 0 && g_tiles.cursorTy >= 0 &&
        tile_in_bounds(g_tiles.cursorTx, g_tiles.cursorTy)) {
        int sx = engine_world_to_screen_x(g_tiles.cursorTx * ts);
        int sy = engine_world_to_screen_y(g_tiles.cursorTy * ts);
        int sw = engine_world_to_screen_len(ts);
        int sh = engine_world_to_screen_len(ts);
        engine_outline_rect(sx, sy, sw, sh, 2, 255, 255, 255);
    }
}

extern "C" int engine_tilemap_save(const char* path) {
    if (!path || g_tiles.w <= 0) return 0;
    std::ofstream f(path, std::ios::binary);
    if (!f) return 0;
    f << "EYESCIRE_TILEMAP 1\n";
    f << "SIZE " << g_tiles.w << " " << g_tiles.h << " " << g_tiles.tile << "\n";
    f << "SKY " << g_tiles.skyR << " " << g_tiles.skyG << " " << g_tiles.skyB << "\n";
    f << "DATA\n";
    f.write(reinterpret_cast<const char*>(g_tiles.cells.data()),
            (std::streamsize)g_tiles.cells.size());
    return f.good() ? 1 : 0;
}

extern "C" int engine_tilemap_load(const char* path) {
    if (!path) return 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::string line;
    int w = 0, h = 0, ts = 16;
    int header_ok = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("EYESCIRE_TILEMAP", 0) == 0) { header_ok = 1; continue; }
        if (line.rfind("SIZE ", 0) == 0) {
            std::istringstream ss(line.substr(5));
            ss >> w >> h >> ts;
            continue;
        }
        if (line.rfind("SKY ", 0) == 0) {
            std::istringstream ss(line.substr(4));
            ss >> g_tiles.skyR >> g_tiles.skyG >> g_tiles.skyB;
            continue;
        }
        if (line.rfind("DATA", 0) == 0) break;
        break;
    }
    if (!header_ok || w <= 0 || h <= 0) return 0;
    engine_tilemap_create(w, h, ts);
    f.read(reinterpret_cast<char*>(g_tiles.cells.data()),
           (std::streamsize)g_tiles.cells.size());
    return f ? 1 : 0;
}

/* ============================================================ */
/*  World generation (Terraria-style staged create)             */
/* ============================================================ */

namespace {

enum {
    WG_AIR = 0,
    WG_GRASS = 1,
    WG_DIRT = 2,
    WG_STONE = 3,
    WG_COPPER = 4,
    WG_WOOD = 5,
    WG_LEAVES = 6,
    WG_SAND = 7,
    WG_ASH = 8,
    WG_IRON = 9,
    WG_GOLD = 10,
    WG_CLAY = 11,
    WG_WATER = 12,
    WG_LAVA = 13,
    WG_SNOW = 14,
    WG_ICE = 15,
    WG_MUD = 16,
    WG_JGRASS = 17,
    WG_GRAVEL = 18,
    WG_FLOWER = 19,
    WG_CACTUS = 20,
    WG_VINE = 21,
    WG_PINE = 22,
    WG_TILE_COUNT = 23
};

/* Surface biome bands, chosen per column from low-frequency noise. */
enum {
    BIO_FOREST = 0,
    BIO_PLAINS,
    BIO_DESERT,
    BIO_SNOW,
    BIO_JUNGLE,
    BIO_OCEAN,
    BIO_COUNT
};

enum {
    ST_SETTLE = 0,
    ST_SHAPE,        /* height field + biome map, smoothed                 */
    ST_TERRAIN,      /* fill columns: surface / soil / stone               */
    ST_WATER,        /* oceans + inland lakes                             */
    ST_CAVES,        /* cheese caverns from 2D noise                      */
    ST_TUNNELS,      /* winding worm tunnels + surface entrances          */
    ST_HELL,         /* ash underworld + lava sea                         */
    ST_ORES,         /* ore veins by depth band                           */
    ST_GRASS,        /* re-grass exposed soil                             */
    ST_TREES,        /* biome trees / cacti / pines                       */
    ST_DECOR,        /* flowers, vines, tufts, spawn pick                 */
    ST_DONE,
    ST_COUNT
};

static const char* kGenStages[ST_COUNT] = {
    "Settling...",
    "Raising the land...",
    "Generating terrain...",
    "Filling the seas...",
    "Hollowing caverns...",
    "Carving tunnels...",
    "Generating underworld...",
    "Seeding ore veins...",
    "Spreading grass...",
    "Planting trees...",
    "Scattering life...",
    "Saving world..."
};

static const char* kGenTips[] = {
    "A pickaxe will break stone and ore.",
    "Falling too far can really hurt.",
    "Chop trees for wood to build a home.",
    "Copper veins hide just below the dirt.",
    "The underworld burns with ancient ash.",
    "Murkwood grows thick with twisted trees.",
    "Leave the seed blank for a surprise.",
    "Larger worlds hold more secrets.",
    "Grass spreads across bare dirt.",
    "Dig straight down at your own risk.",
    "Gold sleeps in the deepest stone.",
    "Build a shelter before you explore.",
    "Deserts hide their water underground.",
    "Snow muffles the sound of footsteps.",
    "Jungle mud slows a careless traveller.",
    "Cave mouths often open near cliffs.",
    "Gravel crumbles when you least want it.",
    "Lava and wood make a poor pairing."
};
static const int kGenTipCount = (int)(sizeof(kGenTips) / sizeof(kGenTips[0]));

static const int kGenWeight[ST_COUNT] = { 2, 4, 14, 5, 16, 14, 7, 10, 5, 12, 6, 2 };

struct WorldGen {
    int active = 0;
    int stage = 0;
    int cursor = 0;
    int w = 0;
    int h = 0;
    int seed = 0;
    int theme = 0;
    int size_idx = 0;
    int tunnel_target = 0;
    int vein_target = 0;
    int steps = 0;
    int tip_i = 0;
    int sea_y = 0;          /* water line for oceans                      */
    int ocean_w = 0;        /* width of the ocean band at each edge       */
    int soil_top = 0;       /* shallowest surface row produced            */
    int stone_end = 0;      /* first ash row                              */
    int spawn_x = 0;
    int spawn_y = 0;
    std::vector<int> height;
    std::vector<uint8_t> biome;
};

WorldGen g_gen;

static void wg_file_log(const char* msg) {
    std::string path;
    if (!g.projRoot.empty()) path = (fs::path(g.projRoot) / "vasterra_gen.log").string();
    else path = "vasterra_gen.log";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "ab") != 0 || !f) return;
    fprintf(f, "%s stage=%d cursor=%d w=%d h=%d\n",
            msg, g_gen.stage, g_gen.cursor, g_gen.w, g_gen.h);
    fclose(f);
}

static int wg_hash_seed(const char* s) {
    if (!s || !s[0]) {
        auto n = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return (int)(n & 0x7fffffff);
    }
    int numeric = 1;
    unsigned acc = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') numeric = 0;
        acc = acc * 10u + (unsigned)(*p - '0');
    }
    if (numeric) return (int)(acc & 0x7fffffff);
    unsigned h = 2166136261u;
    for (const char* p = s; *p; ++p) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return (int)(h & 0x7fffffff);
}

/* ---- noise -------------------------------------------------------------
   engine_noise1 is hardwired to a 16-column wavelength with linear blending,
   which is why the old terrain looked like torn paper: its loudest octave was
   also its shortest. These helpers take the wavelength as an argument and
   blend with a smoothstep, so amplitude can be tied to wavelength properly.
   All of them return 0..1023.                                            */

static int wg_lerp(int a, int b, int t, int scale) {
    return a + (b - a) * t / scale;
}

/* smoothstep on a 0..scale parameter, returned on the same range */
static int wg_smooth_t(int t, int scale) {
    if (scale < 1) return 0;
    if (t <= 0) return 0;
    if (t >= scale) return scale;
    /* 3t^2 - 2t^3 evaluated in 64-bit to keep the fixed point honest */
    long long tt = (long long)t;
    long long s = (long long)scale;
    long long num = tt * tt * (3 * s - 2 * tt);
    return (int)(num / (s * s));
}

/* 1D value noise with an arbitrary wavelength */
static int wg_wave1(int x, int seed, int wavelength) {
    if (wavelength < 2) wavelength = 2;
    int cell = x / wavelength;
    int rem = x - cell * wavelength;
    if (rem < 0) { rem += wavelength; cell -= 1; }
    int a = (int)(noise_hash(cell, 0, seed) & 1023);
    int b = (int)(noise_hash(cell + 1, 0, seed) & 1023);
    return wg_lerp(a, b, wg_smooth_t(rem, wavelength), wavelength);
}

/* 2D value noise with an arbitrary cell size */
static int wg_wave2(int x, int y, int seed, int cell_w, int cell_h) {
    if (cell_w < 2) cell_w = 2;
    if (cell_h < 2) cell_h = 2;
    int cx = x / cell_w;
    int rx = x - cx * cell_w;
    if (rx < 0) { rx += cell_w; cx -= 1; }
    int cy = y / cell_h;
    int ry = y - cy * cell_h;
    if (ry < 0) { ry += cell_h; cy -= 1; }
    int tx = wg_smooth_t(rx, cell_w);
    int ty = wg_smooth_t(ry, cell_h);
    int a = (int)(noise_hash(cx, cy, seed) & 1023);
    int b = (int)(noise_hash(cx + 1, cy, seed) & 1023);
    int c = (int)(noise_hash(cx, cy + 1, seed) & 1023);
    int d = (int)(noise_hash(cx + 1, cy + 1, seed) & 1023);
    int top = wg_lerp(a, b, tx, cell_w);
    int bot = wg_lerp(c, d, tx, cell_w);
    return wg_lerp(top, bot, ty, cell_h);
}

/* Fractal 2D noise, `oct` octaves halving in size and amplitude. 0..1023. */
static int wg_fbm2(int x, int y, int seed, int cell_w, int cell_h, int oct) {
    int sum = 0;
    int amp = 512;
    int total = 0;
    for (int i = 0; i < oct; ++i) {
        sum += (wg_wave2(x, y, seed + i * 131, cell_w, cell_h) - 512) * amp / 512;
        total += amp;
        amp = amp / 2;
        cell_w = (cell_w > 3) ? cell_w / 2 : 2;
        cell_h = (cell_h > 3) ? cell_h / 2 : 2;
        if (amp < 1) break;
    }
    if (total < 1) total = 1;
    int v = 512 + sum * 512 / total;
    if (v < 0) v = 0;
    if (v > 1023) v = 1023;
    return v;
}

/* ---- biomes ----------------------------------------------------------- */

static int wg_biome_at(int x) {
    if (x < 0 || x >= (int)g_gen.biome.size()) return BIO_FOREST;
    return g_gen.biome[(size_t)x];
}

/* Relative surface roughness per biome, in percent. Deserts and plains are
   gentle, snow peaks are dramatic. Blended across columns afterwards so the
   boundaries do not step. */
static int wg_biome_relief(int b) {
    switch (b) {
    case BIO_PLAINS: return 45;
    case BIO_DESERT: return 55;
    case BIO_SNOW:   return 130;
    case BIO_JUNGLE: return 85;
    case BIO_OCEAN:  return 30;
    default:         return 100;
    }
}

/* Which biomes a realm theme grows, as a weighted table.

   The roll comes from smooth noise, so consecutive columns get adjacent
   table entries: the table order *is* the neighbour order. Keep it running
   cold -> temperate -> hot, or you get snowfields butting straight up
   against desert. */
static int wg_pick_biome(int roll, int theme) {
    /* roll is 0..1023 */
    if (theme == 2) {              /* Murkwood: damp, overgrown            */
        if (roll < 120) return BIO_SNOW;
        if (roll < 430) return BIO_FOREST;
        if (roll < 820) return BIO_JUNGLE;
        return BIO_PLAINS;
    }
    if (theme == 3) {              /* Emberdeep: parched, no snow          */
        if (roll < 130) return BIO_FOREST;
        if (roll < 430) return BIO_PLAINS;
        return BIO_DESERT;
    }
    /* Sunlit: classic mix, cold to hot                                    */
    if (roll < 170) return BIO_SNOW;
    if (roll < 450) return BIO_FOREST;
    if (roll < 700) return BIO_PLAINS;
    if (roll < 850) return BIO_JUNGLE;
    return BIO_DESERT;
}

/* ---- height field ------------------------------------------------------
   Built in one pass so it can be smoothed and slope-limited before any
   tiles exist. Amplitude falls with wavelength (the opposite of the old
   wg_surface), which is what keeps hills walkable.                       */

static void wg_build_heightfield() {
    int w = g_gen.w;
    int h = g_gen.h;
    if (w < 8) return;

    g_gen.height.assign((size_t)w, h / 3);
    g_gen.biome.assign((size_t)w, (uint8_t)BIO_FOREST);

    g_gen.ocean_w = w / 14;
    if (g_gen.ocean_w < 18) g_gen.ocean_w = 18;

    /* biome map: one slab every ~w/9 columns, jittered */
    int slab = w / 9;
    if (slab < 60) slab = 60;
    for (int x = 0; x < w; ++x) {
        if (x < g_gen.ocean_w || x >= w - g_gen.ocean_w) {
            g_gen.biome[(size_t)x] = (uint8_t)BIO_OCEAN;
            continue;
        }
        int roll = wg_wave1(x, g_gen.seed + 501, slab);
        g_gen.biome[(size_t)x] = (uint8_t)wg_pick_biome(roll, g_gen.theme);
    }

    /* per-column relief factor, box-blurred so biome seams are gradual */
    std::vector<int> relief((size_t)w, 100);
    for (int x = 0; x < w; ++x) relief[(size_t)x] = wg_biome_relief(wg_biome_at(x));
    {
        int rad = slab / 6;
        if (rad < 12) rad = 12;
        std::vector<int> pre((size_t)w + 1, 0);
        for (int x = 0; x < w; ++x) pre[(size_t)x + 1] = pre[(size_t)x] + relief[(size_t)x];
        std::vector<int> out((size_t)w, 100);
        for (int x = 0; x < w; ++x) {
            int a = x - rad; if (a < 0) a = 0;
            int b = x + rad; if (b > w - 1) b = w - 1;
            out[(size_t)x] = (pre[(size_t)b + 1] - pre[(size_t)a]) / (b - a + 1);
        }
        relief.swap(out);
    }

    /* Continent shape: long wavelength carries the most amplitude. */
    int base = h * 30 / 100;
    if (g_gen.theme == 3) base = h * 33 / 100;

    int wl1 = w / 7;   if (wl1 < 90) wl1 = 90;
    int wl2 = w / 21;  if (wl2 < 40) wl2 = 40;
    int wl3 = w / 60;  if (wl3 < 18) wl3 = 18;
    int wl4 = 11;

    int a1 = h * 100 / 1000;
    int a2 = h *  46 / 1000;
    int a3 = h *  20 / 1000;
    int a4 = h *   8 / 1000;

    for (int x = 0; x < w; ++x) {
        int n1 = wg_wave1(x, g_gen.seed + 11, wl1) - 512;
        int n2 = wg_wave1(x, g_gen.seed + 23, wl2) - 512;
        int n3 = wg_wave1(x, g_gen.seed + 37, wl3) - 512;
        int n4 = wg_wave1(x, g_gen.seed + 53, wl4) - 512;
        int rel = relief[(size_t)x];
        int y = base
              + (n1 * a1 / 512) * rel / 100
              + (n2 * a2 / 512) * rel / 100
              + (n3 * a3 / 512) * rel / 100
              + (n4 * a4 / 512) * rel / 100;
        g_gen.height[(size_t)x] = y;
    }

    /* Sea level sits a little under the mean land height so most coastline
       is beach rather than cliff. */
    g_gen.sea_y = base + h * 42 / 1000;

    /* Ocean basins: ramp the shelf down to a floor well below sea level. */
    int shelf = g_gen.ocean_w;
    int floor_y = g_gen.sea_y + h * 80 / 1000;
    if (floor_y > h * 60 / 100) floor_y = h * 60 / 100;
    for (int i = 0; i < shelf; ++i) {
        int t = wg_smooth_t(i, shelf);               /* 0 at edge, shelf at shore */
        int lx = i;
        int rx = w - 1 - i;
        int land_l = g_gen.height[(size_t)lx];
        int land_r = g_gen.height[(size_t)rx];
        int yl = wg_lerp(floor_y, land_l, t, shelf);
        int yr = wg_lerp(floor_y, land_r, t, shelf);
        int wob = (wg_wave1(lx, g_gen.seed + 71, 23) - 512) * (h * 6 / 1000) / 512;
        int wob2 = (wg_wave1(rx, g_gen.seed + 79, 23) - 512) * (h * 6 / 1000) / 512;
        g_gen.height[(size_t)lx] = yl + wob;
        g_gen.height[(size_t)rx] = yr + wob2;
    }

    /* Smooth, then slope-limit. Two smoothing passes knock the corners off
       the value-noise lattice; the limiter guarantees nothing steeper than
       kMaxStep tiles per column survives, so every hill is climbable. */
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<int> tmp = g_gen.height;
        for (int x = 1; x < w - 1; ++x) {
            g_gen.height[(size_t)x] =
                (tmp[(size_t)x - 1] + tmp[(size_t)x] * 2 + tmp[(size_t)x + 1]) / 4;
        }
    }

    const int kMaxStep = 2;
    for (int pass = 0; pass < 3; ++pass) {
        for (int x = 1; x < w; ++x) {
            int d = g_gen.height[(size_t)x] - g_gen.height[(size_t)x - 1];
            if (d > kMaxStep) g_gen.height[(size_t)x] = g_gen.height[(size_t)x - 1] + kMaxStep;
            else if (d < -kMaxStep) g_gen.height[(size_t)x] = g_gen.height[(size_t)x - 1] - kMaxStep;
        }
        for (int x = w - 2; x >= 0; --x) {
            int d = g_gen.height[(size_t)x] - g_gen.height[(size_t)x + 1];
            if (d > kMaxStep) g_gen.height[(size_t)x] = g_gen.height[(size_t)x + 1] + kMaxStep;
            else if (d < -kMaxStep) g_gen.height[(size_t)x] = g_gen.height[(size_t)x + 1] - kMaxStep;
        }
    }

    /* Deliberate cliffs: a few hard steps keep the smoothed world from
       feeling like a bedsheet. Raising a plateau leaves a true wall on its
       leading edge, while the trailing edge ramps back down. */
    int cliffs = 2 + g_gen.size_idx * 2;
    int inner_lo = g_gen.ocean_w + 40;
    int inner_hi = w - g_gen.ocean_w - 80;
    if (inner_hi > inner_lo) {
        for (int i = 0; i < cliffs; ++i) {
            int cx = inner_lo + (int)(noise_hash(i, 61, g_gen.seed) % (uint32_t)(inner_hi - inner_lo));
            if (wg_biome_at(cx) == BIO_OCEAN) continue;
            int drop = 5 + (int)(noise_hash(i, 67, g_gen.seed) % 7);
            int span = 26 + (int)(noise_hash(i, 71, g_gen.seed) % 46);
            int tail = 22;
            for (int x = cx; x < cx + span && x < w; ++x) {
                g_gen.height[(size_t)x] -= drop;
            }
            for (int t = 0; t < tail; ++t) {
                int x = cx + span + t;
                if (x >= w) break;
                g_gen.height[(size_t)x] -= drop * (tail - t) / tail;
            }
        }
    }

    int top_limit = h * 8 / 100;
    int bot_limit = h * 62 / 100;
    g_gen.soil_top = h;
    for (int x = 0; x < w; ++x) {
        int y = g_gen.height[(size_t)x];
        if (y < top_limit) y = top_limit;
        if (y > bot_limit) y = bot_limit;
        g_gen.height[(size_t)x] = y;
        if (y < g_gen.soil_top) g_gen.soil_top = y;
    }
}

/* ---- palette ----------------------------------------------------------- */

/* Asset paths in the palette are resolved against the project root when one
   is open, so a textured tile does not depend on the process CWD. Falls back
   to the relative path, and to the flat colour if the file is missing. */
static std::string wg_asset(const char* rel) {
    if (g.projLoaded && !g.projRoot.empty()) {
        fs::path p = fs::path(g.projRoot) / rel;
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();
    }
    return std::string(rel);
}

/* The three surrounded frames of Terraria's standard block layout. Only the
   enclosed case needs naming; the other fifteen are read off the art. */
static void wg_autotile_enclosed(int id) {
    engine_tilemap_autotile_frame(id, 0, 1, 1);
    engine_tilemap_autotile_frame(id, 0, 2, 1);
    engine_tilemap_autotile_frame(id, 0, 3, 1);
}

static void wg_define_tiles() {
    engine_tilemap_define(WG_AIR,     0,   0,   0,   0, "");
    /* Grass and dirt autotile off Terraria-format sheets: the frame is chosen
       per cell from which sides are open, so grass grows a crown along the top
       of a run and caps off at its ends. Dirt-Tilesheet.png is the grass sheet
       with the green repainted (tools/make_dirt_tilesheet.py), which is what
       lets a grass block sit on a dirt block with no seam - a grass block is a
       dirt block with grass on its exposed faces. */
    engine_tilemap_define(WG_GRASS,  86, 166,  58,  1, "");
    engine_tilemap_sheet_autotile(WG_GRASS,
                         wg_asset("assets/Grass-Tilesheet.png").c_str(),
                         16, 18, 13, 5);
    wg_autotile_enclosed(WG_GRASS);
    engine_tilemap_define(WG_DIRT,  140,  92,  52,  1, "");
    engine_tilemap_sheet_autotile(WG_DIRT,
                         wg_asset("assets/Dirt-Tilesheet.png").c_str(),
                         16, 18, 13, 5);
    wg_autotile_enclosed(WG_DIRT);
    engine_tilemap_define(WG_STONE, 118, 118, 124,  1, "");
    engine_tilemap_define(WG_COPPER,196, 110,  42,  1, "");
    /* Foliage is walk-through, as in Terraria: with trees this dense, solid
       trunks turn the surface into a maze of walls the ledge assist cannot
       climb. */
    engine_tilemap_define(WG_WOOD,  118,  78,  42,  0, "");
    engine_tilemap_define(WG_LEAVES, 46, 122,  48,  0, "");
    engine_tilemap_define(WG_SAND,  214, 186, 106,  1, "");
    engine_tilemap_define(WG_ASH,    92,  36,  28,  1, "");
    engine_tilemap_define(WG_IRON,  168, 172, 180,  1, "");
    engine_tilemap_define(WG_GOLD,  212, 168,  42,  1, "");
    engine_tilemap_define(WG_CLAY,  164,  84,  58,  1, "");
    engine_tilemap_define(WG_WATER,  54, 108, 200,  0, "");
    engine_tilemap_define(WG_LAVA,  214,  92,  28,  0, "");
    engine_tilemap_define(WG_SNOW,  232, 238, 246,  1, "");
    engine_tilemap_define(WG_ICE,   168, 206, 230,  1, "");
    engine_tilemap_define(WG_MUD,    94,  66,  44,  1, "");
    engine_tilemap_define(WG_JGRASS, 58, 148,  46,  1, "");
    engine_tilemap_define(WG_GRAVEL,132, 126, 118,  1, "");
    engine_tilemap_define(WG_FLOWER,214,  92, 120,  0, "");
    engine_tilemap_define(WG_CACTUS, 74, 140,  70,  0, "");
    engine_tilemap_define(WG_VINE,   52, 116,  48,  0, "");
    engine_tilemap_define(WG_PINE,   30,  92,  62,  0, "");
}

static int wg_theme_sky_r(int theme) { return (theme == 2) ? 62 : (theme == 3) ? 118 : 92; }
static int wg_theme_sky_g(int theme) { return (theme == 2) ? 86 : (theme == 3) ?  72 : 168; }
static int wg_theme_sky_b(int theme) { return (theme == 2) ? 108 : (theme == 3) ? 52 : 228; }

/* ---- column fill ------------------------------------------------------- */

static int wg_surface_block(int b) {
    switch (b) {
    case BIO_DESERT: return WG_SAND;
    case BIO_SNOW:   return WG_SNOW;
    case BIO_JUNGLE: return WG_JGRASS;
    case BIO_OCEAN:  return WG_SAND;
    default:         return WG_GRASS;
    }
}

static int wg_soil_block(int b, int depth) {
    switch (b) {
    case BIO_DESERT: return (depth < 14) ? WG_SAND : WG_DIRT;
    case BIO_SNOW:   return (depth < 3) ? WG_SNOW : WG_DIRT;
    case BIO_JUNGLE: return WG_MUD;
    case BIO_OCEAN:  return (depth < 5) ? WG_SAND : WG_DIRT;
    default:         return WG_DIRT;
    }
}

static int wg_is_soil_id(int id) {
    return id == WG_DIRT || id == WG_SAND || id == WG_MUD ||
           id == WG_SNOW || id == WG_GRASS || id == WG_JGRASS || id == WG_CLAY;
}

static void wg_fill_column(int x) {
    int h = g_gen.h;
    int sy = g_gen.height[(size_t)x];
    int b = wg_biome_at(x);
    int stone_end = g_gen.stone_end;

    int soil = 15 + wg_wave1(x, g_gen.seed + 8, 29) * 14 / 1023;
    if (b == BIO_DESERT) soil += 10;
    if (b == BIO_JUNGLE) soil += 7;
    if (b == BIO_SNOW)   soil += 3;

    /* Beaches: coastal ground near the waterline turns sandy, so the sea
       meets a shore instead of a grass cliff. Pond shores get the same
       treatment later, where the pond is actually placed. */
    int dsea = sy - g_gen.sea_y;
    if (dsea < 0) dsea = -dsea;
    int coast = g_gen.ocean_w + 36;
    int near_coast = (x < coast || x >= g_gen.w - coast);
    int beach = 0;
    if (b == BIO_OCEAN) beach = 1;
    else if (near_coast && dsea <= 5) beach = 1;

    for (int y = sy; y < stone_end; ++y) {
        int depth = y - sy;
        int id;
        if (depth < soil) {
            id = beach && depth < 7 ? WG_SAND : wg_soil_block(b, depth);
        } else {
            int band = 7;
            if (depth < soil + band) {
                /* ragged soil/stone seam instead of a ruler-straight line */
                int n = wg_wave2(x, y, g_gen.seed + 91, 9, 5);
                int t = (depth - soil) * 1023 / band;
                id = (n > t) ? wg_soil_block(b, depth) : WG_STONE;
            } else {
                id = WG_STONE;
            }
        }
        tile_set(x, y, id);
    }

    int top = beach ? WG_SAND : wg_surface_block(b);
    tile_set(x, sy, top);
}

/* ---- water ------------------------------------------------------------- */

/* Flood coastal ground that sits below the waterline. Restricted to the
   shelf plus a little inland so the sea and its bays fill, without the
   waterline drowning every inland valley that happens to dip low. */
static void wg_water_column(int x) {
    int coast = g_gen.ocean_w + 36;
    if (x >= coast && x < g_gen.w - coast) return;
    int sy = g_gen.height[(size_t)x];
    if (sy <= g_gen.sea_y) return;
    for (int y = g_gen.sea_y; y < sy; ++y) {
        if (tile_get(x, y) == WG_AIR) tile_set(x, y, WG_WATER);
    }
}

/* A pond sits in a closed basin above sea level: find the low point, pick a
   level a few tiles up, walk out to the rim and only fill if the basin is
   actually enclosed and a sane size. */
static void wg_make_pond(int cx) {
    int w = g_gen.w;
    int coast = g_gen.ocean_w + 40;
    if (cx < coast || cx >= w - coast) return;      /* the sea handles those */
    if (wg_biome_at(cx) == BIO_OCEAN) return;

    int best = cx;
    for (int x = cx - 20; x <= cx + 20; ++x) {
        if (x < 1 || x >= w - 1) continue;
        if (g_gen.height[(size_t)x] > g_gen.height[(size_t)best]) best = x;
    }
    int floor_y = g_gen.height[(size_t)best];
    int depth = 3 + (int)(noise_hash(cx, 41, g_gen.seed) % 4);
    int level = floor_y - depth;
    if (level < 4) return;

    int l = best;
    while (l - 1 >= 1 && g_gen.height[(size_t)l - 1] > level) --l;
    int r = best;
    while (r + 1 < w - 1 && g_gen.height[(size_t)r + 1] > level) ++r;

    int width = r - l + 1;
    if (width < 5 || width > 90) return;
    if (g_gen.height[(size_t)l - 1] > level) return; /* open on the left  */
    if (g_gen.height[(size_t)r + 1] > level) return; /* open on the right */

    for (int x = l; x <= r; ++x) {
        int sy = g_gen.height[(size_t)x];
        for (int y = level; y < sy; ++y) {
            if (tile_get(x, y) == WG_AIR) tile_set(x, y, WG_WATER);
        }
    }
    /* sandy bed and shore */
    for (int x = l - 2; x <= r + 2; ++x) {
        if (x < 0 || x >= w) continue;
        int sy = g_gen.height[(size_t)x];
        for (int y = sy; y < sy + 3; ++y) {
            int id = tile_get(x, y);
            if (wg_is_soil_id(id)) tile_set(x, y, WG_SAND);
        }
    }
}

/* ---- caves ------------------------------------------------------------- */

static int wg_carvable(int id) {
    return id == WG_DIRT || id == WG_STONE || id == WG_SAND || id == WG_MUD ||
           id == WG_CLAY || id == WG_GRAVEL || id == WG_SNOW || id == WG_GRASS ||
           id == WG_JGRASS || id == WG_COPPER || id == WG_IRON || id == WG_GOLD;
}

static int wg_column_surface(int x) {
    if (x < 0 || x >= g_gen.w || x >= (int)g_gen.height.size()) return g_gen.h / 3;
    int sy = g_gen.height[(size_t)x];
    if (sy < 1) sy = 1;
    if (sy > g_gen.h - 2) sy = g_gen.h - 2;
    return sy;
}

/* Keep a roof of solid ground under the surface so caves never open the
   whole world up from above, and never touch standing water. */
static int wg_cave_roof(int x) {
    return wg_column_surface(x) + 7;
}

static void wg_carve_disk(int cx, int cy, int r) {
    int r2 = r * r + 1;
    for (int y = cy - r; y <= cy + r; ++y) {
        if (y < 6 || y >= g_gen.stone_end) continue;
        for (int x = cx - r; x <= cx + r; ++x) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy > r2) continue;
            if (x < 1 || x >= g_gen.w - 1) continue;
            if (y < wg_cave_roof(x)) continue;
            if (!wg_carvable(tile_get(x, y))) continue;
            tile_set(x, y, WG_AIR);
        }
    }
}

/* Cheese caverns: threshold a stretched 3-octave field so the holes are
   wider than they are tall, and open up with depth. */
static void wg_cheese_column(int x) {
    int roof = wg_cave_roof(x);
    int bottom = g_gen.stone_end;
    if (roof >= bottom) return;
    int span = bottom - roof;
    if (span < 1) span = 1;
    for (int y = roof; y < bottom; ++y) {
        int id = tile_get(x, y);
        if (!wg_carvable(id)) continue;
        int t = (y - roof) * 1023 / span;
        /* tight near the roof, opening up towards the ash line */
        int thresh = 812 - t * 104 / 1023;
        int n = wg_fbm2(x, y * 2, g_gen.seed + 301, 30, 15, 3);
        if (n > thresh) tile_set(x, y, WG_AIR);
    }
}

/* Winding tunnel. The angle turns by a small random amount each step rather
   than snapping to one of eight directions, which is what made the old
   worms look like pencil strokes. */
static void wg_tunnel(int index, int from_surface) {
    int w = g_gen.w;
    int h = g_gen.h;
    if (w < 60) return;

    int margin = 10;
    float x = (float)(margin + (int)(noise_hash(index, 3, g_gen.seed) % (uint32_t)(w - 2 * margin)));
    float y;
    float ang;
    int life;
    float rad = 1.5f + (float)(noise_hash(index, 17, g_gen.seed) % 150) / 100.0f;

    if (from_surface) {
        int sx = (int)x;
        y = (float)(wg_cave_roof(sx) + 2);
        /* aim down, leaning left or right */
        float lean = (float)((int)(noise_hash(index, 19, g_gen.seed) % 120) - 60) / 100.0f;
        ang = 1.5708f + lean;
        life = 34 + (int)(noise_hash(index, 23, g_gen.seed) % 50);
    } else {
        int y0 = h * 34 / 100;
        int y1 = h * 82 / 100;
        int span = y1 - y0;
        if (span < 1) span = 1;
        y = (float)(y0 + (int)(noise_hash(index, 7, g_gen.seed) % (uint32_t)span));
        ang = (float)(noise_hash(index, 13, g_gen.seed) % 628) / 100.0f;
        life = 55 + (int)(noise_hash(index, 11, g_gen.seed) % 95);
    }

    int min_y = h * 18 / 100;
    int max_y = g_gen.stone_end - 2;

    for (int i = 0; i < life; ++i) {
        wg_carve_disk((int)x, (int)y, (int)(rad + 0.5f));

        /* smooth turn, plus a mild downward bias so tunnels tend to descend */
        int turn = (int)(noise_hash(index * 977 + i, 31, g_gen.seed) % 1000);
        ang += (float)(turn - 500) / 1150.0f;
        ang += 0.012f * ((y < (float)(h * 55 / 100)) ? 1.0f : -0.35f);

        if ((noise_hash(index * 131 + i, 37, g_gen.seed) % 23) == 0) {
            rad = 1.4f + (float)(noise_hash(i, index, g_gen.seed + 9) % 160) / 100.0f;
        }

        x += cosf(ang);
        y += sinf(ang);
        if (x < 6.0f || x >= (float)(w - 6) || y < (float)min_y || y >= (float)max_y) break;
    }
}

/* ---- ore veins ---------------------------------------------------------
   Random-walked blobs instead of a global noise threshold, so ore arrives in
   veins you can follow rather than confetti. */
static int wg_ore_host(int id) {
    return id == WG_STONE || id == WG_DIRT || id == WG_SAND ||
           id == WG_MUD || id == WG_SNOW;
}

static void wg_vein(int index, int ore, int y0, int y1, int steps_min, int steps_max, int rad_max) {
    int w = g_gen.w;
    if (w < 40) return;
    if (y1 <= y0) return;
    int x = 8 + (int)(noise_hash(index, 211, g_gen.seed) % (uint32_t)(w - 16));
    int y = y0 + (int)(noise_hash(index, 223, g_gen.seed) % (uint32_t)(y1 - y0));
    /* keep the seed underground for this column, or the whole vein is wasted
       walking through open sky above a valley */
    int under = wg_column_surface(x) + 10;
    if (y < under) y = under;
    if (y >= y1) return;
    int span = steps_max - steps_min;
    if (span < 1) span = 1;
    int steps = steps_min + (int)(noise_hash(index, 227, g_gen.seed) % (uint32_t)span);
    float ang = (float)(noise_hash(index, 229, g_gen.seed) % 628) / 100.0f;

    for (int i = 0; i < steps; ++i) {
        int r = 1 + (int)(noise_hash(index * 733 + i, 233, g_gen.seed) % (uint32_t)rad_max);
        int r2 = r * r + 1;
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r2) continue;
                int tx = x + dx;
                int ty = y + dy;
                if (ty < y0 - 4 || ty > y1 + 4) continue;
                if (wg_ore_host(tile_get(tx, ty))) tile_set(tx, ty, ore);
            }
        }
        ang += (float)((int)(noise_hash(index * 811 + i, 239, g_gen.seed) % 1000) - 500) / 700.0f;
        x += (int)(cosf(ang) * 1.6f);
        y += (int)(sinf(ang) * 1.6f);
        if (x < 4 || x >= w - 4) break;
        if (y < y0 - 4 || y > y1 + 4) break;
    }
}

/* ---- underworld -------------------------------------------------------- */

static void wg_hell_column(int x) {
    int h = g_gen.h;
    int top = g_gen.stone_end;
    for (int y = top; y < h; ++y) tile_set(x, y, WG_ASH);

    /* cavern roof with stalactites, floor above a lava sea */
    int ceil_y = top + 4 + (wg_wave1(x, g_gen.seed + 77, 17) * (h * 22 / 1000)) / 1023;
    int lava_y = h * 955 / 1000 + (wg_wave1(x, g_gen.seed + 83, 29) * (h * 8 / 1000)) / 1023;
    if (lava_y > h - 2) lava_y = h - 2;
    if (ceil_y >= lava_y) ceil_y = lava_y - 3;
    if (ceil_y < top + 1) ceil_y = top + 1;

    for (int y = ceil_y; y < lava_y; ++y) tile_set(x, y, WG_AIR);
    for (int y = lava_y; y < h; ++y) tile_set(x, y, WG_LAVA);

    /* the odd ash spike hanging from the ceiling */
    if ((noise_hash(x, 57, g_gen.seed) % 13) == 0) {
        int len = 2 + (int)(noise_hash(x, 59, g_gen.seed) % 5);
        for (int i = 0; i < len && ceil_y + i < lava_y; ++i) tile_set(x, ceil_y + i, WG_ASH);
    }
}

/* ---- grass ------------------------------------------------------------- */

/* First tile that is neither air nor water: the real walkable surface after
   caves and lakes have had their say. */
static int wg_find_surface(int x) {
    for (int y = 0; y < g_gen.h; ++y) {
        int id = tile_get(x, y);
        if (id == WG_AIR || id == WG_WATER) continue;
        return y;
    }
    return g_gen.h;
}

static void wg_grass_column(int x) {
    int sy = wg_find_surface(x);
    if (sy <= 0 || sy >= g_gen.h) return;
    g_gen.height[(size_t)x] = sy;
    int above = tile_get(x, sy - 1);
    if (above == WG_WATER) return;           /* underwater: no grass */
    int id = tile_get(x, sy);
    int b = wg_biome_at(x);
    if (id == WG_DIRT) {
        if (b == BIO_JUNGLE) tile_set(x, sy, WG_JGRASS);
        else if (b == BIO_SNOW) tile_set(x, sy, WG_SNOW);
        else tile_set(x, sy, WG_GRASS);
    } else if (id == WG_MUD && b == BIO_JUNGLE) {
        tile_set(x, sy, WG_JGRASS);
    }
}

/* ---- trees ------------------------------------------------------------- */

static int wg_tree_clear(int x, int height_needed, int half_width) {
    for (int dx = -half_width; dx <= half_width; ++dx) {
        int xx = x + dx;
        if (xx < 0 || xx >= g_gen.w) continue;
        int sy = wg_column_surface(xx);
        int y0 = sy - height_needed - 2;
        if (y0 < 0) y0 = 0;
        for (int y = y0; y < sy; ++y) {
            int id = tile_get(xx, y);
            if (id == WG_WOOD || id == WG_LEAVES || id == WG_PINE ||
                id == WG_CACTUS || id == WG_WATER) return 0;
        }
    }
    return 1;
}

static void wg_leaf_blob(int cx, int cy, int r, int leaf) {
    int r2 = r * r + 1;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r2) continue;
            int tx = cx + dx;
            int ty = cy + dy;
            if (ty < 0 || tx < 0 || tx >= g_gen.w) continue;
            if (tile_get(tx, ty) == WG_AIR) tile_set(tx, ty, leaf);
        }
    }
}

static void wg_tree_broadleaf(int x, int sy) {
    int th = 6 + (int)(noise_hash(x, 9, g_gen.seed) % 8);
    if (sy - th < 3) th = sy - 3;
    if (th < 5) return;
    if (!wg_tree_clear(x, th, 3)) return;

    for (int i = 1; i <= th; ++i) tile_set(x, sy - i, WG_WOOD);
    int top = sy - th;

    /* a branch or two part way up */
    int branches = (int)(noise_hash(x, 29, g_gen.seed) % 3);
    for (int bi = 0; bi < branches; ++bi) {
        int by = top + 2 + (int)(noise_hash(x, 31 + bi, g_gen.seed) % (uint32_t)std::max(1, th - 3));
        int dir = (noise_hash(x, 37 + bi, g_gen.seed) & 1) ? 1 : -1;
        int len = 1 + (int)(noise_hash(x, 41 + bi, g_gen.seed) % 2);
        for (int i = 1; i <= len; ++i) {
            if (tile_get(x + dir * i, by) == WG_AIR) tile_set(x + dir * i, by, WG_WOOD);
        }
        wg_leaf_blob(x + dir * (len + 1), by, 2, WG_LEAVES);
    }

    int r = (th >= 10) ? 4 : 3;
    wg_leaf_blob(x, top, r, WG_LEAVES);
    wg_leaf_blob(x, top + 1, r - 1, WG_LEAVES);
}

static void wg_tree_pine(int x, int sy) {
    int th = 8 + (int)(noise_hash(x, 9, g_gen.seed) % 9);
    if (sy - th < 3) th = sy - 3;
    if (th < 6) return;
    if (!wg_tree_clear(x, th, 3)) return;

    for (int i = 1; i <= th; ++i) tile_set(x, sy - i, WG_WOOD);
    int top = sy - th;
    /* Stacked skirts, widest at the bottom. The top two tiers overwrite the
       trunk so the tip is a point rather than a bare pole; below that the
       trunk column is left alone so the stem still reads. */
    int tiers = 3 + th / 4;
    for (int t = 0; t < tiers; ++t) {
        int ty = top + t * 2;
        if (ty >= sy) break;
        int half = t;
        if (half > 3) half = 3;
        for (int dx = -half; dx <= half; ++dx) {
            int tx = x + dx;
            if (dx == 0) {
                if (t < 2) tile_set(tx, ty, WG_PINE);
                continue;
            }
            if (tile_get(tx, ty) == WG_AIR) tile_set(tx, ty, WG_PINE);
            if (tile_get(tx, ty + 1) == WG_AIR) tile_set(tx, ty + 1, WG_PINE);
        }
    }
}

static void wg_tree_jungle(int x, int sy) {
    int th = 10 + (int)(noise_hash(x, 9, g_gen.seed) % 10);
    if (sy - th < 4) th = sy - 4;
    if (th < 8) return;
    if (!wg_tree_clear(x, th, 5)) return;

    /* two-wide trunk */
    for (int i = 1; i <= th; ++i) {
        tile_set(x, sy - i, WG_WOOD);
        tile_set(x + 1, sy - i, WG_WOOD);
    }
    int top = sy - th;
    wg_leaf_blob(x, top, 5, WG_LEAVES);
    wg_leaf_blob(x + 1, top + 2, 4, WG_LEAVES);

    /* vines dangling out of the canopy */
    for (int dx = -4; dx <= 5; ++dx) {
        int tx = x + dx;
        if (tx < 0 || tx >= g_gen.w) continue;
        if ((noise_hash(tx, 47, g_gen.seed) % 3) != 0) continue;
        int vy = top + 4;
        if (tile_get(tx, vy) != WG_LEAVES) continue;
        int len = 2 + (int)(noise_hash(tx, 53, g_gen.seed) % 5);
        for (int i = 1; i <= len; ++i) {
            if (tile_get(tx, vy + i) != WG_AIR) break;
            tile_set(tx, vy + i, WG_VINE);
        }
    }
}

static void wg_cactus(int x, int sy) {
    int th = 2 + (int)(noise_hash(x, 9, g_gen.seed) % 4);
    if (sy - th < 3) return;
    if (!wg_tree_clear(x, th, 2)) return;
    for (int i = 1; i <= th; ++i) tile_set(x, sy - i, WG_CACTUS);
    if (th >= 3 && (noise_hash(x, 61, g_gen.seed) % 3) == 0) {
        int ay = sy - th + 1;
        int dir = (noise_hash(x, 67, g_gen.seed) & 1) ? 1 : -1;
        if (tile_get(x + dir, ay) == WG_AIR) tile_set(x + dir, ay, WG_CACTUS);
        if (tile_get(x + dir, ay - 1) == WG_AIR) tile_set(x + dir, ay - 1, WG_CACTUS);
    }
}

static void wg_plant_column(int x) {
    if (x < 8 || x >= g_gen.w - 8) return;

    /* leave the spawn area open */
    int away = x - g_gen.w / 2;
    if (away < 0) away = -away;
    if (away < 14) return;

    int b = wg_biome_at(x);
    if (b == BIO_OCEAN) return;

    int sy = wg_column_surface(x);
    int ground = tile_get(x, sy);
    if (tile_get(x, sy - 1) != WG_AIR) return;      /* submerged or roofed */

    int chance;
    switch (b) {
    case BIO_JUNGLE: chance = 240; break;
    case BIO_FOREST: chance = 200; break;
    case BIO_SNOW:   chance = 160; break;
    case BIO_PLAINS: chance = 70;  break;
    case BIO_DESERT: chance = 55;  break;
    default:         chance = 110; break;
    }
    if (g_gen.theme == 2) chance += 40;
    if (g_gen.theme == 3) chance -= 15;

    int roll = (int)(noise_hash(x, 1, g_gen.seed) % 1000);
    if (roll >= chance) return;

    if (b == BIO_DESERT) {
        if (ground != WG_SAND) return;
        wg_cactus(x, sy);
    } else if (b == BIO_SNOW) {
        if (ground != WG_SNOW && ground != WG_DIRT) return;
        wg_tree_pine(x, sy);
    } else if (b == BIO_JUNGLE) {
        if (ground != WG_JGRASS && ground != WG_MUD) return;
        wg_tree_jungle(x, sy);
    } else {
        if (ground != WG_GRASS && ground != WG_DIRT) return;
        wg_tree_broadleaf(x, sy);
    }
}

/* ---- decoration -------------------------------------------------------- */

static void wg_decor_column(int x) {
    int sy = wg_column_surface(x);
    if (sy < 2 || sy >= g_gen.h - 1) return;
    int ground = tile_get(x, sy);
    if (tile_get(x, sy - 1) != WG_AIR) return;

    int roll = (int)(noise_hash(x, 33, g_gen.seed) % 1000);
    if (ground == WG_GRASS) {
        if (roll > 880) tile_set(x, sy - 1, WG_FLOWER);
        else if (roll > 700) tile_set(x, sy - 1, WG_LEAVES);
    } else if (ground == WG_JGRASS) {
        if (roll > 760) tile_set(x, sy - 1, WG_LEAVES);
        else if (roll > 690) tile_set(x, sy - 1, WG_FLOWER);
    } else if (ground == WG_SNOW) {
        /* frozen patches in the snow itself; ice hanging in the air above it
           just looked like floating cubes */
        if (roll > 860) tile_set(x, sy, WG_ICE);
    }
}

/* Somewhere flat, dry and open near the middle of the map. */
static void wg_pick_spawn() {
    int w = g_gen.w;
    int mid = w / 2;
    int best = mid;
    int best_score = -1;

    int reach = w / 4;
    if (reach > 400) reach = 400;
    for (int off = 0; off < reach; ++off) {
        for (int s = 0; s < 2; ++s) {
            int x = mid + (s ? -off : off);
            if (x < 12 || x >= w - 12) continue;
            int sy = wg_find_surface(x);
            if (sy < 4 || sy >= g_gen.h - 4) continue;
            int ground = tile_get(x, sy);
            if (ground == WG_WATER || ground == WG_LAVA) continue;
            if (!wg_is_soil_id(ground) && ground != WG_STONE) continue;
            if (tile_get(x, sy - 1) != WG_AIR) continue;
            if (tile_get(x, sy - 2) != WG_AIR) continue;

            /* prefer flat: penalise height change over a short span */
            int rough = 0;
            for (int d = -4; d <= 4; ++d) {
                int xx = x + d;
                if (xx < 0 || xx >= w) continue;
                int diff = wg_find_surface(xx) - sy;
                if (diff < 0) diff = -diff;
                rough += diff;
            }
            int score = 1000 - rough * 30 - off;
            if (score > best_score) {
                best_score = score;
                best = x;
            }
            if (rough == 0) { off = w; break; }      /* perfect, stop looking */
        }
    }

    int sy = wg_find_surface(best);
    if (sy < 3) sy = 3;
    g_gen.spawn_x = best;
    g_gen.spawn_y = sy - 3;
    if (g_gen.spawn_y < 1) g_gen.spawn_y = 1;

    /* make sure the spawn tile is standable */
    if (!wg_is_soil_id(tile_get(best, sy)) && tile_get(best, sy) != WG_STONE) {
        for (int d = -2; d <= 2; ++d) tile_set(best + d, sy, WG_DIRT);
    }
}

/* ---- preview ----------------------------------------------------------- */

static void wg_preview_rgb(int tx, int ty, int* r, int* g_, int* b) {
    uint8_t id = tile_get(tx, ty);
    if (id == WG_AIR) {
        int surf = g_tiles.h / 3;
        if (tx >= 0 && tx < (int)g_gen.height.size()) surf = g_gen.height[(size_t)tx];
        if (ty >= g_gen.stone_end) {
            *r = 42; *g_ = 14; *b = 10;
            return;
        }
        if (ty >= surf) {
            *r = 16; *g_ = 14; *b = 20;
            return;
        }
        *r = g_tiles.skyR;
        *g_ = g_tiles.skyG;
        *b = g_tiles.skyB;
        return;
    }
    const TileDef& d = g_tiles.defs[id];
    *r = d.r;
    *g_ = d.g;
    *b = d.b;
}

static int wg_stage_frac() {
    int denom = 1;
    int cur = g_gen.cursor;
    switch (g_gen.stage) {
    case ST_SETTLE:
    case ST_SHAPE:
        denom = 1;
        break;
    case ST_TERRAIN:
    case ST_WATER:
    case ST_CAVES:
    case ST_HELL:
    case ST_GRASS:
    case ST_TREES:
    case ST_DECOR:
        denom = std::max(1, g_gen.w);
        break;
    case ST_TUNNELS:
        denom = std::max(1, g_gen.tunnel_target);
        break;
    case ST_ORES:
        denom = std::max(1, g_gen.vein_target);
        break;
    case ST_DONE:
        return kGenWeight[ST_DONE];
    default:
        break;
    }
    if (cur < 0) cur = 0;
    if (cur > denom) cur = denom;
    return kGenWeight[g_gen.stage] * cur / denom;
}

} // namespace

/* Shared so the game script can restore the palette after tilemap_load,
   which brings back cells and sky but not the tile table. */
extern "C" void engine_world_tiles_define() {
    wg_define_tiles();
}

extern "C" void engine_world_gen_begin(int size_idx, int theme_idx) {
    g_gen = WorldGen{};
    g_gen.active = 1;
    g_tilemap_draw_hidden = 1;
    if (size_idx < 0) size_idx = 0;
    if (size_idx > 2) size_idx = 2;
    g_gen.size_idx = size_idx;
    g_gen.seed = wg_hash_seed(g_editSeed);
    int theme = theme_idx;
    if (theme < 0 || theme > 3) theme = 0;
    if (theme == 0) theme = 1 + (g_gen.seed % 3);
    g_gen.theme = theme;
    if (size_idx == 0) { g_gen.w = 840; g_gen.h = 240; }
    else if (size_idx == 1) { g_gen.w = 1680; g_gen.h = 360; }
    else { g_gen.w = 2520; g_gen.h = 480; }
    g_gen.stone_end = g_gen.h * 86 / 100;
    g_gen.stage = ST_SETTLE;
    g_gen.cursor = 0;
    g_gen.tip_i = (int)(noise_hash(g_gen.seed, 1, 99) % (uint32_t)kGenTipCount);

    int tunnels = 16 + size_idx * 16;
    if (g_gen.theme == 2) tunnels += tunnels / 3;
    if (g_gen.theme == 3) tunnels += tunnels / 5;
    g_gen.tunnel_target = tunnels;
    g_gen.vein_target = 90 + size_idx * 130;

    wg_file_log("begin");
    engine_log("[vasterra] world gen begin");
}

extern "C" int engine_world_gen_active() {
    return g_gen.active ? 1 : 0;
}

extern "C" int engine_world_gen_progress() {
    if (!g_gen.active) return 0;
    int total = 0;
    int done = 0;
    for (int i = 0; i < ST_COUNT; ++i) total += kGenWeight[i];
    for (int i = 0; i < g_gen.stage; ++i) done += kGenWeight[i];
    done += wg_stage_frac();
    if (total < 1) total = 1;
    int pct = done * 100 / total;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

extern "C" const char* engine_world_gen_stage() {
    if (!g_gen.active) return "Settling...";
    int s = g_gen.stage;
    if (s < 0) s = 0;
    if (s >= ST_COUNT) s = ST_DONE;
    return kGenStages[s];
}

extern "C" const char* engine_world_gen_tip() {
    int i = g_gen.tip_i;
    if (i < 0 || i >= kGenTipCount) i = 0;
    return kGenTips[i];
}

extern "C" void engine_world_gen_draw_preview(int x, int y, int w, int h) {
    if (!g.backDC || w < 8 || h < 8) return;
    if (g_tiles.w <= 0 || g_tiles.h <= 0) {
        engine_fill_rect(x, y, w, h, 12, 14, 22);
        return;
    }
    static std::vector<uint32_t> pix;
    size_t need = (size_t)w * (size_t)h;
    if (pix.size() != need) pix.assign(need, 0xFF0C0E16u);
    for (int py = 0; py < h; ++py) {
        int ty = py * g_tiles.h / h;
        if (ty < 0) ty = 0;
        if (ty >= g_tiles.h) ty = g_tiles.h - 1;
        for (int px = 0; px < w; ++px) {
            int tx = px * g_tiles.w / w;
            if (tx < 0) tx = 0;
            if (tx >= g_tiles.w) tx = g_tiles.w - 1;
            int cr, cg, cb;
            wg_preview_rgb(tx, ty, &cr, &cg, &cb);
            /* BI_RGB 32bpp is BGRA in memory, so blue goes in the low byte;
               packing red there swapped the preview's channels (blue lava,
               orange sky). */
            pix[(size_t)py * (size_t)w + (size_t)px] =
                0xFF000000u | ((uint32_t)cb) | ((uint32_t)cg << 8) | ((uint32_t)cr << 16);
        }
    }
    {
        Gdiplus::Graphics gfx(g.backDC);
        gfx.Flush(Gdiplus::FlushIntentionSync);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(g.backDC, x, y, (DWORD)w, (DWORD)h, 0, 0, 0, (UINT)h,
                      pix.data(), &bi, DIB_RGB_COLORS);
}

extern "C" int engine_world_gen_step() {
    if (!g_gen.active) return -1;
    g_gen.steps += 1;
    if ((g_gen.steps % 160) == 0) {
        g_gen.tip_i = (g_gen.tip_i + 1) % kGenTipCount;
    }

    switch (g_gen.stage) {
    case ST_SETTLE: {
        if (engine_tilemap_create(g_gen.w, g_gen.h, 16) == 0) {
            g_gen.active = 0;
            g_tilemap_draw_hidden = 0;
            return -1;
        }
        wg_define_tiles();
        engine_tilemap_set_sky(wg_theme_sky_r(g_gen.theme),
                               wg_theme_sky_g(g_gen.theme),
                               wg_theme_sky_b(g_gen.theme));
        g_gen.height.assign((size_t)g_gen.w, g_gen.h / 3);
        g_gen.biome.assign((size_t)g_gen.w, (uint8_t)BIO_FOREST);
        g_gen.stage = ST_SHAPE;
        g_gen.cursor = 0;
        return 0;
    }
    case ST_SHAPE: {
        wg_build_heightfield();
        g_gen.stage = ST_TERRAIN;
        g_gen.cursor = 0;
        wg_file_log("shape done");
        return 0;
    }
    case ST_TERRAIN: {
        int end = std::min(g_gen.w, g_gen.cursor + 56);
        for (int x = g_gen.cursor; x < end; ++x) wg_fill_column(x);
        g_gen.cursor = end;
        if (g_gen.cursor >= g_gen.w) {
            g_gen.stage = ST_WATER;
            g_gen.cursor = 0;
            wg_file_log("terrain done");
        }
        return 0;
    }
    case ST_WATER: {
        int end = std::min(g_gen.w, g_gen.cursor + 96);
        for (int x = g_gen.cursor; x < end; ++x) {
            wg_water_column(x);
            /* pond seeds on a coarse grid, gated by noise */
            if ((x % 23) == 0 && (noise_hash(x, 101, g_gen.seed) % 100) < 46) wg_make_pond(x);
        }
        g_gen.cursor = end;
        if (g_gen.cursor >= g_gen.w) {
            g_gen.stage = ST_CAVES;
            g_gen.cursor = 0;
            wg_file_log("water done");
        }
        return 0;
    }
    case ST_CAVES: {
        int end = std::min(g_gen.w, g_gen.cursor + 24);
        for (int x = g_gen.cursor; x < end; ++x) wg_cheese_column(x);
        g_gen.cursor = end;
        if (g_gen.cursor >= g_gen.w) {
            g_gen.stage = ST_TUNNELS;
            g_gen.cursor = 0;
            wg_file_log("caves done");
        }
        return 0;
    }
    case ST_TUNNELS: {
        int done = 0;
        while (done < 2 && g_gen.cursor < g_gen.tunnel_target) {
            int surface = (g_gen.cursor % 5) == 0 ? 1 : 0;
            wg_tunnel(g_gen.cursor, surface);
            g_gen.cursor += 1;
            done += 1;
        }
        if (g_gen.cursor >= g_gen.tunnel_target) {
            g_gen.stage = ST_HELL;
            g_gen.cursor = 0;
            wg_file_log("tunnels done");
        }
        return 0;
    }
    case ST_HELL: {
        if (g_gen.cursor == 0) wg_file_log("hell start");
        int end = std::min(g_gen.w, g_gen.cursor + 64);
        for (int x = g_gen.cursor; x < end; ++x) wg_hell_column(x);
        g_gen.cursor = end;
        if (g_gen.cursor >= g_gen.w) {
            g_gen.stage = ST_ORES;
            g_gen.cursor = 0;
            wg_file_log("hell done");
        }
        return 0;
    }
    case ST_ORES: {
        int h = g_gen.h;
        int y_soil = g_gen.soil_top + 12;
        int y_mid  = h * 48 / 100;
        int y_deep = h * 68 / 100;
        int y_ash  = g_gen.stone_end;
        int total = g_gen.vein_target;

        int done = 0;
        while (done < 6 && g_gen.cursor < total) {
            int i = g_gen.cursor;
            int slot = i % 10;
            if (slot <= 3)      wg_vein(i, WG_COPPER, y_soil, y_mid,  7, 16, 2);
            else if (slot <= 5) wg_vein(i, WG_IRON,   y_mid,  y_deep, 6, 14, 2);
            else if (slot == 6) wg_vein(i, WG_GOLD,   y_deep, y_ash,  4, 10, 2);
            else if (slot == 7) wg_vein(i, WG_CLAY,   y_soil, y_mid,  8, 18, 3);
            else                wg_vein(i, WG_GRAVEL, y_mid,  y_ash,  6, 14, 3);
            g_gen.cursor += 1;
            done += 1;
        }
        if (g_gen.cursor >= total) {
            g_gen.stage = ST_GRASS;
            g_gen.cursor = 0;
            wg_file_log("ores done");
        }
        return 0;
    }
    case ST_GRASS: {
        int end = std::min(g_gen.w, g_gen.cursor + 96);
        for (int x = g_gen.cursor; x < end; ++x) wg_grass_column(x);
        g_gen.cursor = end;
        if (g_gen.cursor >= g_gen.w) {
            g_gen.stage = ST_TREES;
            g_gen.cursor = 0;
            wg_file_log("grass done");
        }
        return 0;
    }
    case ST_TREES: {
        if (g_gen.cursor == 0) wg_file_log("trees start");
        int end = std::min(g_gen.w, g_gen.cursor + 32);
        for (int x = g_gen.cursor; x < end; ++x) wg_plant_column(x);
        g_gen.cursor = end;
        if (g_gen.cursor >= g_gen.w) {
            g_gen.stage = ST_DECOR;
            g_gen.cursor = 0;
            wg_file_log("trees done");
        }
        return 0;
    }
    case ST_DECOR: {
        int end = std::min(g_gen.w, g_gen.cursor + 96);
        for (int x = g_gen.cursor; x < end; ++x) wg_decor_column(x);
        g_gen.cursor = end;
        if (g_gen.cursor >= g_gen.w) {
            wg_pick_spawn();
            g_gen.stage = ST_DONE;
            g_gen.cursor = 1;
            wg_file_log("decor done");
        }
        return 0;
    }
    case ST_DONE:
        wg_file_log("done");
        return 1;
    default:
        return -1;
    }
}

extern "C" int engine_worlds_finish_gen(int size_idx, int diff_idx, int theme_idx) {
    wg_file_log("finish start");
    g_tilemap_draw_hidden = 0;
    if (!g_gen.active || g_tiles.w <= 0) {
        wg_file_log("finish inactive");
        engine_tilemap_destroy();
        g_gen = WorldGen{};
        return -1;
    }
    int idx = engine_worlds_create_from_edit(size_idx, diff_idx, theme_idx);
    if (idx < 0) {
        wg_file_log("finish create failed");
        engine_tilemap_destroy();
        g_gen = WorldGen{};
        return -1;
    }
    fs::path dir = fs::path(worlds_root_dir()) / g.worldNames[(size_t)idx];
    fs::path map = dir / "world.map";
    wg_file_log("finish saving");
    int saved = engine_tilemap_save(map.string().c_str());
    if (!saved) {
        wg_file_log("finish save failed");
        engine_worlds_delete(idx);
        engine_tilemap_destroy();
        g_gen = WorldGen{};
        return -1;
    }
    /* ST_DECOR picked a flat, dry, open spot; fall back to the middle column
       if the generator never got that far. */
    int spawn_x = g_gen.spawn_x;
    int spawn_y = g_gen.spawn_y;
    if (spawn_x <= 0 || spawn_x >= g_gen.w || spawn_y <= 0) {
        spawn_x = g_gen.w / 2;
        spawn_y = 8;
        if (spawn_x >= 0 && spawn_x < (int)g_gen.height.size()) {
            spawn_y = g_gen.height[(size_t)spawn_x] - 3;
            if (spawn_y < 1) spawn_y = 1;
        }
    }
    fs::path cfg = dir / "world.cfg";
    FILE* f = nullptr;
    if (fopen_s(&f, cfg.string().c_str(), "ab") == 0 && f) {
        fprintf(f, "width=%d\nheight=%d\nspawn_x=%d\nspawn_y=%d\n",
                g_gen.w, g_gen.h, spawn_x, spawn_y);
        fclose(f);
    }
    engine_tilemap_destroy();
    g_gen = WorldGen{};
    wg_file_log("finish ok");
    engine_log("[vasterra] world gen saved");
    return idx;
}

#include "blueprint.inl"
#include "sfx.inl"
#include "sky.inl"
#include "particles.inl"
#include "render3d.inl"
