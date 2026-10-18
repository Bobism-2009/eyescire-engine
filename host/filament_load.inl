/* Loads filament_bridge.dll from beside engine.dll. Filament is MSVC C++;
   this file only calls it through the C API. */

static HMODULE g_filMod = nullptr;
static int g_filTried = 0;

static int (*p_fb_init)(void*) = nullptr;
static void (*p_fb_shutdown)() = nullptr;
static int (*p_fb_ok)() = nullptr;
static const char* (*p_fb_last_error)() = nullptr;
static void (*p_fb_begin)(float, float, float, float, float, float, float, float, float,
                          float, float, int, int, int, int, int, int, int, int, int, int, int, float) = nullptr;
static void (*p_fb_sun)(float, float, float, float, float, float, float, int) = nullptr;
static void (*p_fb_point)(float, float, float, float, float, float, float, float) = nullptr;
static void (*p_fb_spot)(float, float, float, float, float, float, float, float, float, float, float, float, float) = nullptr;
static void (*p_fb_ambient)(float, float, float) = nullptr;
static void (*p_fb_sky)(const unsigned char* const*, const int*, const char*) = nullptr;
static void (*p_fb_prim)(int, const float*, int, int, int, int, int, int) = nullptr;
static void (*p_fb_mat_params)(float, float, float, float, float) = nullptr;
static void (*p_fb_prim_maps)(int, const float*, int, int, int, int, int, int,
                              const unsigned char*, int, int, const unsigned char*, int, int,
                              const unsigned char*, int, int, const unsigned char*, int, int,
                              const unsigned char*, int, int) = nullptr;
static void (*p_fb_model)(const void*, const float*, const float*, const float*, int, const float*,
                          int, int, int, int, int, const unsigned char*, int, int,
                          const unsigned char*, int, int, const unsigned char*, int, int,
                          const unsigned char*, int, int, const unsigned char*, int, int) = nullptr;
static void (*p_fb_model_forget)(const void*) = nullptr;
static void (*p_fb_tris)(const float*, const float*, int, const float*, int, int, int, int, int, int, int, int) = nullptr;
static void (*p_fb_line)(float, float, float, float, float, float, int, int, int, int, int) = nullptr;
static void (*p_fb_sprite)(const void*, const unsigned char*, int, int, const float*, const float*, int, int, int, int) = nullptr;
static void (*p_fb_screen)(float, float, float, float, int, int, int, int) = nullptr;
static int (*p_fb_project)(float, float, float, float*, float*) = nullptr;
static void (*p_fb_overlay)(const void*, int, int) = nullptr;
static int (*p_fb_draw)(int) = nullptr;
static int (*p_fb_take_capture)(unsigned char*, int, int*, int*) = nullptr;

static int fil_bind() {
    if (g_filMod) return p_fb_init ? 1 : 0;
    if (g_filTried) return 0;
    g_filTried = 1;
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void*)&fil_bind, &self))
        return 0;
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    slash[1] = 0;
    size_t used = (size_t)(slash - path) + 1;
    if (used + 22 >= MAX_PATH) return 0;
    wcscpy(path + used, L"filament_bridge.dll");
    g_filMod = LoadLibraryW(path);
    if (!g_filMod) return 0;
    auto sym = [](const char* name) { return GetProcAddress(g_filMod, name); };
    p_fb_init = (decltype(p_fb_init))sym("fb_init");
    p_fb_shutdown = (decltype(p_fb_shutdown))sym("fb_shutdown");
    p_fb_ok = (decltype(p_fb_ok))sym("fb_ok");
    p_fb_last_error = (decltype(p_fb_last_error))sym("fb_last_error");
    p_fb_begin = (decltype(p_fb_begin))sym("fb_begin");
    p_fb_sun = (decltype(p_fb_sun))sym("fb_sun");
    p_fb_point = (decltype(p_fb_point))sym("fb_point");
    p_fb_spot = (decltype(p_fb_spot))sym("fb_spot");
    p_fb_ambient = (decltype(p_fb_ambient))sym("fb_ambient");
    p_fb_sky = (decltype(p_fb_sky))sym("fb_sky");
    p_fb_prim = (decltype(p_fb_prim))sym("fb_prim");
    p_fb_mat_params = (decltype(p_fb_mat_params))sym("fb_mat_params");
    p_fb_prim_maps = (decltype(p_fb_prim_maps))sym("fb_prim_maps");
    p_fb_model = (decltype(p_fb_model))sym("fb_model");
    p_fb_model_forget = (decltype(p_fb_model_forget))sym("fb_model_forget");
    p_fb_tris = (decltype(p_fb_tris))sym("fb_tris");
    p_fb_line = (decltype(p_fb_line))sym("fb_line");
    p_fb_sprite = (decltype(p_fb_sprite))sym("fb_sprite");
    p_fb_screen = (decltype(p_fb_screen))sym("fb_screen");
    p_fb_project = (decltype(p_fb_project))sym("fb_project");
    p_fb_overlay = (decltype(p_fb_overlay))sym("fb_overlay");
    p_fb_draw = (decltype(p_fb_draw))sym("fb_draw");
    p_fb_take_capture = (decltype(p_fb_take_capture))sym("fb_take_capture");
    if (!p_fb_init || !p_fb_shutdown || !p_fb_begin || !p_fb_draw || !p_fb_prim || !p_fb_model) {
        FreeLibrary(g_filMod);
        g_filMod = nullptr;
        return 0;
    }
    return 1;
}

static void fil_shutdown() {
    if (p_fb_shutdown) p_fb_shutdown();
}

static int fil_init(void* hwnd) {
    if (!fil_bind()) return 0;
    return p_fb_init(hwnd);
}

static const char* fil_error() {
    if (p_fb_last_error) return p_fb_last_error();
    if (!g_filMod) return "filament_bridge.dll was not next to engine.dll";
    return "filament bridge failed";
}
