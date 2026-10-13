/* Content encryption for shipped game files.
   ChaCha20 (+ auth / dual-pass on higher tiers), decrypt-once on load.

   0 Off | 1 Standard | 2 Strong | 3 Secure (studio). Deterrence, not DRM.

   Included into engine.cpp. */

extern uint8_t g_eyescire_ksplit[32];
extern uint8_t g_eyescire_ksplit2[32];

namespace content_crypt {

/* Legacy markers — still accepted so older exports keep working. */
static constexpr char kLegacyFileMagic[8] = { 'E','Y','E','E','N','C','0','1' };
static constexpr char kLegacyKeyMagic[8]  = { 'E','Y','E','K','E','Y','0','1' };
static constexpr char kLegacyExeTail[8]   = { 'E','Y','E','K','E','N','D','1' };
static constexpr size_t kLegacyEmbedSize  = 8 + 32 + 8;

/* Trailer: mask[32] + wrapped[32] + crc[4] + stamp[4] */
static constexpr size_t kEmbedSize = 32 + 32 + 4 + 4;
static constexpr uint32_t kEmbedStamp = 0xC3A5E91Bu;       /* Standard */
static constexpr uint32_t kEmbedStampStrong = 0xD4B6F02Au; /* Strong */
static constexpr uint32_t kEmbedStampSecure = 0xE5C7083Du; /* Secure */

static uint8_t g_key_enc[32]; /* key XOR g_key_mask */
static uint8_t g_key_mask[32];
static int g_key_ok = 0;
static int g_protect = 1; /* 0..3 */
static int g_auth = 0;    /* require integrity tag */
static int g_secure = 0;  /* per-file keys + dual ChaCha pass */
static uint8_t g_fp_expect[16];
static int g_fp_ok = 0;
static DWORD g_fp_last_check = 0;
static std::string g_fp_dll_path;
static std::string g_fp_exe_path;
static std::unordered_map<std::string, std::string> g_tempBySrc;

static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void chacha_quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

static void chacha20_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64]) {
    uint32_t s[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) {
        s[4 + i] = (uint32_t)key[i * 4] | ((uint32_t)key[i * 4 + 1] << 8) |
                   ((uint32_t)key[i * 4 + 2] << 16) | ((uint32_t)key[i * 4 + 3] << 24);
    }
    s[12] = counter;
    s[13] = (uint32_t)nonce[0] | ((uint32_t)nonce[1] << 8) | ((uint32_t)nonce[2] << 16) | ((uint32_t)nonce[3] << 24);
    s[14] = (uint32_t)nonce[4] | ((uint32_t)nonce[5] << 8) | ((uint32_t)nonce[6] << 16) | ((uint32_t)nonce[7] << 24);
    s[15] = (uint32_t)nonce[8] | ((uint32_t)nonce[9] << 8) | ((uint32_t)nonce[10] << 16) | ((uint32_t)nonce[11] << 24);
    uint32_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = s[i];
    for (int i = 0; i < 10; ++i) {
        chacha_quarter(w[0], w[4], w[8], w[12]);
        chacha_quarter(w[1], w[5], w[9], w[13]);
        chacha_quarter(w[2], w[6], w[10], w[14]);
        chacha_quarter(w[3], w[7], w[11], w[15]);
        chacha_quarter(w[0], w[5], w[10], w[15]);
        chacha_quarter(w[1], w[6], w[11], w[12]);
        chacha_quarter(w[2], w[7], w[8], w[13]);
        chacha_quarter(w[3], w[4], w[9], w[14]);
    }
    for (int i = 0; i < 16; ++i) {
        uint32_t v = w[i] + s[i];
        out[i * 4] = (uint8_t)v;
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t* in, uint8_t* out, size_t len,
                         uint32_t counter_start = 1) {
    uint32_t counter = counter_start;
    size_t off = 0;
    while (off < len) {
        uint8_t block[64];
        chacha20_block(key, nonce, counter++, block);
        size_t n = len - off;
        if (n > 64) n = 64;
        for (size_t i = 0; i < n; ++i) out[off + i] = in[off + i] ^ block[i];
        off += n;
    }
}

/* Strong-tier integrity: ChaCha20 PRF tag over ciphertext (fast, no extra deps). */
static void auth_tag(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t* ct, size_t n, uint8_t out[16]) {
    uint8_t n2[12];
    std::memcpy(n2, nonce, 12);
    n2[0] ^= 0x5A; /* domain-separate from the encrypt stream */
    uint8_t block[64];
    chacha20_block(key, n2, (uint32_t)n ^ 0x9E3779B9u, block);
    for (size_t i = 0; i < n; ++i) block[i & 63] ^= ct[i];
    uint8_t final[64];
    chacha20_block(key, n2, 0xC2B2AE3Du, final);
    for (int i = 0; i < 16; ++i) out[i] = block[i] ^ final[i] ^ final[16 + i];
}

static void aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                         const uint8_t* plain, size_t n,
                         std::vector<uint8_t>& cipher, uint8_t mac[16]) {
    cipher.resize(n);
    if (n) chacha20_xor(key, nonce, plain, cipher.data(), n, 1);
    auth_tag(key, nonce, cipher.data(), n, mac);
}

static int aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t* ct, size_t n, const uint8_t mac[16],
                        std::vector<uint8_t>& plain) {
    uint8_t expect[16];
    auth_tag(key, nonce, ct, n, expect);
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= expect[i] ^ mac[i];
    if (diff) return 0;
    plain.resize(n);
    if (n) chacha20_xor(key, nonce, ct, plain.data(), n, 1);
    return 1;
}

/* Scattered constants mixed at runtime — not one contiguous key in .rdata. */
static void host_mix(uint8_t out[32]) {
    static const uint32_t p0[] = { 0xA7B3C19Du, 0x51E60F2Au, 0x9C4D83E1u, 0x2F7A14B6u };
    static const uint32_t p1[] = { 0x6D28E5C3u, 0xF01A9B47u, 0x3C85D2E9u, 0x84B17F0Eu };
    static const uint32_t p2[] = { 0x19D4A6C8u, 0xE73B502Fu, 0x5A9EC174u, 0xC2F0683Bu };
    static const uint32_t p3[] = { 0x47E1B89Au, 0x0D5C3F6Eu, 0xB8A247D1u, 0x63F9C05Au };
    uint32_t w[8];
    for (int i = 0; i < 4; ++i) {
        w[i]     = p0[i] ^ rotl32(p1[i], 7)  ^ (p2[3 - i] + 0x9E3779B9u);
        w[4 + i] = p3[i] ^ rotl32(p2[i], 13) ^ (p0[i] * 0x01000193u);
    }
    for (int r = 0; r < 4; ++r) {
        for (int i = 0; i < 8; ++i)
            w[i] = rotl32(w[i] + w[(i + 3) & 7] + 0x7F4A7C15u, (i & 3) + 5) ^ w[(i + 5) & 7];
    }
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = (uint8_t)w[i];
        out[i * 4 + 1] = (uint8_t)(w[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(w[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(w[i] >> 24);
    }
}

/* Separate from host_mix — seals Secure fingerprints so trailer overwrite
   after patching binaries is not enough without this build salt. */
static void build_seal_key(uint8_t out[32]) {
    static const uint32_t p0[] = { 0xD1F07A3Cu, 0x8B2E61F5u, 0x4C90A17Eu, 0xE35B28D9u };
    static const uint32_t p1[] = { 0x27C4D8A1u, 0x9F6B03E4u, 0x5A18C7B2u, 0xC0E94F36u };
    static const uint32_t p2[] = { 0x6E3A91D5u, 0xB7F2048Cu, 0x1D85E6A3u, 0xF24970BEu };
    static const uint32_t p3[] = { 0x83A5C61Fu, 0x4E70D2B9u, 0xA1C38F57u, 0x2F96E4D0u };
    uint32_t w[8];
    for (int i = 0; i < 4; ++i) {
        w[i]     = p0[i] ^ rotl32(p2[i], 11) ^ (p1[3 - i] + 0x7F4A7C15u);
        w[4 + i] = p3[i] ^ rotl32(p1[i], 17) ^ (p0[i] * 0x85EBCA6Bu);
    }
    for (int r = 0; r < 6; ++r) {
        for (int i = 0; i < 8; ++i)
            w[i] = rotl32(w[i] + w[(i + 5) & 7] + 0xC2B2AE3Du, (i & 3) + 3) ^ w[(i + 2) & 7];
    }
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = (uint8_t)w[i];
        out[i * 4 + 1] = (uint8_t)(w[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(w[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(w[i] >> 24);
    }
}

static void wipe(uint8_t* p, size_t n); /* forward */

/* Keyed seal of fingerprint H: trailer stores seal, not raw H.
   Attacker who recomputes H after patching still cannot forge seal without salt. */
static void keyed_fp_seal(const uint8_t fp[16], uint8_t out[16]) {
    uint8_t salt[32], nonce[12];
    build_seal_key(salt);
    /* Domain-separated nonce for trailer seals. */
    static const uint8_t kN[12] = {
        0x54, 0x52, 0x4C, 0x53, 0x45, 0x41, 0x4C, 0x31, 0xC3, 0xA5, 0xE9, 0x1B
    }; /* "TRLSEAL1" + mix */
    std::memcpy(nonce, kN, 12);
    auth_tag(salt, nonce, fp, 16, out);
    wipe(salt, 32);
}

static uint32_t crc32_bytes(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(c & 1u));
    }
    return ~c;
}

static void wipe(uint8_t* p, size_t n) {
    volatile uint8_t* v = p;
    while (n--) *v++ = 0;
}

static int fill_random(uint8_t* dst, size_t n) {
    HMODULE adv = GetModuleHandleA("advapi32.dll");
    if (!adv) adv = LoadLibraryA("advapi32.dll");
    if (adv) {
        auto fn = (BOOLEAN(APIENTRY*)(PVOID, ULONG))GetProcAddress(adv, "SystemFunction036");
        if (fn && fn(dst, (ULONG)n)) return 1;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    uint64_t x = (uint64_t)qpc.QuadPart ^ (uint64_t)GetTickCount64() ^ (uint64_t)GetCurrentProcessId();
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        dst[i] = (uint8_t)x;
    }
    return 1;
}

static void store_key(const uint8_t key[32]) {
    fill_random(g_key_mask, 32);
    for (int i = 0; i < 32; ++i) g_key_enc[i] = key[i] ^ g_key_mask[i];
    g_key_ok = 1;
}

static void reveal_key(uint8_t key[32]) {
    for (int i = 0; i < 32; ++i) key[i] = g_key_enc[i] ^ g_key_mask[i];
}

static std::vector<uint8_t> read_raw(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static int write_raw(const std::string& path, const uint8_t* data, size_t n) {
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return 0;
    if (n) o.write(reinterpret_cast<const char*>(data), (std::streamsize)n);
    return o ? 1 : 0;
}

static std::string norm_rel(const std::string& path) {
    std::string n = path;
    for (char& c : n) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    auto slash = n.find_last_of('/');
    return (slash == std::string::npos) ? n : n.substr(slash + 1);
}

/* 8-byte header unique per (game key, filename). No shared magic across titles. */
static void file_tag(const uint8_t key[32], const std::string& base, uint8_t out[8]) {
    uint32_t a = 0x811C9DC5u;
    uint32_t b = 0x01000193u;
    for (int i = 0; i < 32; ++i) {
        a ^= key[i];
        a *= b;
        b = rotl32(b ^ key[31 - i], 5) + 0x9E3779B9u;
    }
    for (unsigned char c : base) {
        a ^= (uint32_t)c;
        a *= 0x01000193u;
        b ^= rotl32(a, 11) + (uint32_t)c;
    }
    a ^= b; b += rotl32(a, 17);
    out[0] = (uint8_t)a;       out[1] = (uint8_t)(a >> 8);
    out[2] = (uint8_t)(a >> 16); out[3] = (uint8_t)(a >> 24);
    out[4] = (uint8_t)b;       out[5] = (uint8_t)(b >> 8);
    out[6] = (uint8_t)(b >> 16); out[7] = (uint8_t)(b >> 24);
}

static int looks_encrypted(const std::vector<uint8_t>& b, const std::string& path = {}) {
    if (b.size() < 8 + 12) return 0;
    if (std::memcmp(b.data(), kLegacyFileMagic, 8) == 0) return 1;
    if (!g_key_ok || path.empty()) return 0;
    uint8_t key[32], tag[8];
    reveal_key(key);
    file_tag(key, norm_rel(path), tag);
    wipe(key, 32);
    return std::memcmp(b.data(), tag, 8) == 0 ? 1 : 0;
}

static std::string lower_ext(const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string e = path.substr(pos);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e;
}

static int path_has_folder(const std::string& n, const char* folder) {
    std::string needle = std::string("/") + folder + "/";
    if (n.find(needle) != std::string::npos) return 1;
    std::string prefix = std::string(folder) + "/";
    if (n.rfind(prefix, 0) == 0) return 1;
    return 0;
}

static int path_is_payload(const std::string& path) {
    std::string n = path;
    for (char& c : n) if (c == '\\') c = '/';
    for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (n.size() >= 11 && n.compare(n.size() - 11, 11, "content.key") == 0) return 0;
    {
        auto slash = n.find_last_of('/');
        std::string base = (slash == std::string::npos) ? n : n.substr(slash + 1);
        if (base == "game.cfg" || base == "splash.cfg" || base == "extensions.cfg") return 1;
    }
    if (lower_ext(n) == ".eye") return 1;
    if (path_has_folder(n, "scenes") || lower_ext(n) == ".scene") return 1;
    if (path_has_folder(n, "prefabs") || lower_ext(n) == ".prefab") return 1;
    if (path_has_folder(n, "assets")) return 1;
    if (path_has_folder(n, "bin") && lower_ext(n) == ".dll") return 1;
    if (path_has_folder(n, "extensions") && lower_ext(n) == ".dll") return 1;
    return 0;
}

static int path_is_ext_ship(const std::string& path) {
    /* Player builds: DLL only — no extension.json / source metadata. */
    return lower_ext(path) == ".dll" ? 1 : 0;
}

static void put_u32(uint8_t* d, uint32_t v) {
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t* d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static int find_ksplit_payload(const std::vector<uint8_t>& b, const uint8_t mag[32], uint8_t out[16]) {
    if (b.size() < 32) return 0;
    for (size_t i = 0; i + 32 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, mag, 16) == 0) {
            std::memcpy(out, b.data() + i + 16, 16);
            return 1;
        }
    }
    return 0;
}

static int patch_ksplit_file(const char* dll_path, const uint8_t mag[32], const uint8_t half[16]) {
    if (!dll_path || !dll_path[0]) return 0;
    auto b = read_raw(dll_path);
    for (size_t i = 0; i + 32 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, mag, 16) == 0) {
            std::memcpy(b.data() + i + 16, half, 16);
            return write_raw(dll_path, b.data(), b.size());
        }
    }
    return 0;
}

static void clear_temps(); /* forward — used by tamper wipe */

static int read_dll_halves_live(uint8_t out1[16], uint8_t* out2 /* optional */) {
    char path[MAX_PATH];
    HMODULE m = GetModuleHandleA("engine.dll");
    if (!m) return 0;
    DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    auto b = read_raw(path);
    if (!find_ksplit_payload(b, g_eyescire_ksplit, out1)) return 0;
    if (out2 && !find_ksplit_payload(b, g_eyescire_ksplit2, out2)) return 0;
    g_fp_dll_path = path;
    return 1;
}

/* Hash module bytes with key-slot payloads zeroed so the seal covers code, not the shares. */
static void fingerprint_bytes(std::vector<uint8_t> b, uint8_t out[16]) {
    auto zero_slot = [&](const uint8_t mag[32]) {
        for (size_t i = 0; i + 32 <= b.size(); ++i) {
            if (std::memcmp(b.data() + i, mag, 16) == 0) {
                std::memset(b.data() + i + 16, 0, 16);
                return;
            }
        }
    };
    zero_slot(g_eyescire_ksplit);
    zero_slot(g_eyescire_ksplit2);
    uint32_t a = 0x811C9DC5u, c = 0x01000193u, x = 0xA5A5A5A5u, y = 0x3C6EF372u;
    a ^= (uint32_t)b.size();
    for (size_t i = 0; i < b.size(); ++i) {
        a ^= b[i];
        a *= c;
        x = rotl32(x ^ b[i] ^ a, 7) + 0x9E3779B9u;
        y = rotl32(y + b[i] + x, 11) ^ a;
        if ((i & 0x3FF) == 0x3FF) { a ^= x; c = rotl32(c ^ y, 5); }
    }
    put_u32(out, a ^ x);
    put_u32(out + 4, y ^ rotl32(a, 13));
    put_u32(out + 8, x + y);
    put_u32(out + 12, rotl32(a ^ y, 17) ^ (uint32_t)b.size());
}

static int fingerprint_secure_pair(const char* exe_path, const char* dll_path,
                                  size_t exe_body_len, uint8_t out[16]) {
    if (!dll_path || !dll_path[0]) return 0;
    auto dll = read_raw(dll_path);
    if (dll.empty()) return 0;
    uint8_t fd[16], fe[16];
    fingerprint_bytes(std::move(dll), fd);
    if (exe_path && exe_path[0]) {
        auto exe = read_raw(exe_path);
        if (exe.size() < exe_body_len) exe_body_len = exe.size();
        if (exe_body_len > 0) {
            exe.resize(exe_body_len);
            fingerprint_bytes(std::move(exe), fe);
        } else {
            std::memset(fe, 0, 16);
        }
    } else {
        std::memset(fe, 0, 16);
    }
    for (int i = 0; i < 16; ++i) out[i] = fd[i] ^ fe[i] ^ (uint8_t)(0x5C + i);
    return 1;
}

static int secure_fp_verify(int force) {
    if (!g_secure || !g_fp_ok) return g_secure ? 0 : 1;
    DWORD now = GetTickCount();
    if (!force && (now - g_fp_last_check) < 2500u) return 1;
    g_fp_last_check = now;
    if (g_fp_dll_path.empty()) {
        char path[MAX_PATH];
        HMODULE m = GetModuleHandleA("engine.dll");
        if (!m) return 0;
        DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 0;
        g_fp_dll_path = path;
    }
    if (g_fp_exe_path.empty()) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (n && n < MAX_PATH) g_fp_exe_path = path;
    }
    size_t body = 0;
    {
        auto exe = read_raw(g_fp_exe_path);
        if (exe.size() >= kEmbedSize) body = exe.size() - kEmbedSize;
        else body = exe.size();
    }
    uint8_t fp[16], seal[16];
    if (!fingerprint_secure_pair(g_fp_exe_path.c_str(), g_fp_dll_path.c_str(), body, fp))
        return 0;
    keyed_fp_seal(fp, seal);
    wipe(fp, 16);
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= seal[i] ^ g_fp_expect[i];
    wipe(seal, 16);
    if (diff) {
        engine_log("[content] Secure tamper check failed — key wiped");
        wipe(g_key_enc, 32);
        wipe(g_key_mask, 32);
        wipe(g_fp_expect, 16);
        g_key_ok = 0;
        g_fp_ok = 0;
        g_auth = 0;
        g_secure = 0;
        clear_temps();
        return 0;
    }
    return 1;
}

static void derive_file_key(const uint8_t master[32], const std::string& base, uint8_t out[32]) {
    uint8_t tag[8], nonce[12];
    file_tag(master, base, tag);
    std::memset(nonce, 0, 12);
    std::memcpy(nonce, tag, 8);
    nonce[8] = 0x53; nonce[9] = 0x45; nonce[10] = 0x43; nonce[11] = 0x31; /* "SEC1" */
    uint8_t block[64];
    chacha20_block(master, nonce, 0x51EDC0DEu, block);
    std::memcpy(out, block, 32);
    wipe(block, 64);
}

static int load_key_from_exe_bytes(const std::vector<uint8_t>& b) {
    if (b.size() >= kEmbedSize) {
        const uint8_t* t = b.data() + (b.size() - kEmbedSize);
        uint32_t crc = get_u32(t + 64);
        uint32_t stamp = get_u32(t + 68);
        if (crc32_bytes(t, 64) == crc) {
            uint8_t mix[32], key[32];
            host_mix(mix);
            if (stamp == (kEmbedStampSecure ^ crc)) {
                /* Secure: verify keyed fingerprint seal before assembling key. */
                uint8_t h1[16], h2[16], expect_seal[16], fp[16], got_seal[16];
                if (!read_dll_halves_live(h1, h2)) { wipe(mix, 32); return 0; }
                for (int i = 0; i < 16; ++i)
                    expect_seal[i] = t[32 + 16 + i] ^ t[16 + i]; /* sealed fp ^ mask */
                {
                    char exePath[MAX_PATH];
                    DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
                    if (n == 0 || n >= MAX_PATH) {
                        wipe(mix, 32); wipe(h1, 16); wipe(h2, 16); wipe(expect_seal, 16);
                        return 0;
                    }
                    g_fp_exe_path = exePath;
                    size_t body = (b.size() >= kEmbedSize) ? b.size() - kEmbedSize : b.size();
                    if (!fingerprint_secure_pair(exePath, g_fp_dll_path.c_str(), body, fp)) {
                        wipe(mix, 32); wipe(h1, 16); wipe(h2, 16); wipe(expect_seal, 16);
                        return 0;
                    }
                }
                keyed_fp_seal(fp, got_seal);
                wipe(fp, 16);
                uint8_t diff = 0;
                for (int i = 0; i < 16; ++i) diff |= expect_seal[i] ^ got_seal[i];
                wipe(got_seal, 16);
                if (diff) {
                    wipe(mix, 32); wipe(h1, 16); wipe(h2, 16); wipe(expect_seal, 16);
                    engine_log("[content] Secure tamper check failed — refusing key");
                    return 0;
                }
                std::memcpy(g_fp_expect, expect_seal, 16);
                wipe(expect_seal, 16);
                g_fp_ok = 1;
                g_fp_last_check = GetTickCount();
                for (int i = 0; i < 16; ++i) key[i] = t[32 + i] ^ t[i] ^ mix[i];
                for (int i = 0; i < 16; ++i) key[16 + i] = h1[i] ^ h2[i] ^ mix[16 + i];
                wipe(h1, 16); wipe(h2, 16); wipe(mix, 32);
                store_key(key);
                wipe(key, 32);
                g_auth = 1; g_secure = 1; g_protect = 3;
                return 1;
            }
            if (stamp == (kEmbedStampStrong ^ crc)) {
                /* Strong: first 16 from trailer, second 16 from engine.dll slot. */
                uint8_t half[16];
                if (!read_dll_halves_live(half, nullptr)) { wipe(mix, 32); return 0; }
                for (int i = 0; i < 16; ++i) key[i] = t[32 + i] ^ t[i] ^ mix[i];
                for (int i = 0; i < 16; ++i) key[16 + i] = half[i] ^ mix[16 + i];
                wipe(half, 16);
                wipe(mix, 32);
                store_key(key);
                wipe(key, 32);
                g_auth = 1; g_secure = 0; g_protect = 2;
                return 1;
            }
            if (stamp == (kEmbedStamp ^ crc)) {
                for (int i = 0; i < 32; ++i) key[i] = t[32 + i] ^ t[i] ^ mix[i];
                wipe(mix, 32);
                store_key(key);
                wipe(key, 32);
                g_auth = 0; g_secure = 0; g_protect = 1;
                return 1;
            }
            wipe(mix, 32);
        }
    }
    if (b.size() >= kLegacyEmbedSize) {
        const uint8_t* tail = b.data() + (b.size() - kLegacyEmbedSize);
        if (std::memcmp(tail, kLegacyKeyMagic, 8) == 0 &&
            std::memcmp(tail + 40, kLegacyExeTail, 8) == 0) {
            store_key(tail + 8);
            g_auth = 0; g_secure = 0; g_protect = 1;
            return 1;
        }
    }
    return 0;
}

static void strip_embed_trailer(std::vector<uint8_t>& b) {
    if (b.size() >= kEmbedSize) {
        const uint8_t* t = b.data() + (b.size() - kEmbedSize);
        uint32_t crc = get_u32(t + 64);
        uint32_t stamp = get_u32(t + 68);
        if (crc32_bytes(t, 64) == crc &&
            (stamp == (kEmbedStamp ^ crc) || stamp == (kEmbedStampStrong ^ crc) ||
             stamp == (kEmbedStampSecure ^ crc))) {
            b.resize(b.size() - kEmbedSize);
            return;
        }
    }
    if (b.size() >= kLegacyEmbedSize) {
        const uint8_t* tail = b.data() + (b.size() - kLegacyEmbedSize);
        if (std::memcmp(tail, kLegacyKeyMagic, 8) == 0 &&
            std::memcmp(tail + 40, kLegacyExeTail, 8) == 0) {
            b.resize(b.size() - kLegacyEmbedSize);
        }
    }
}

static void clear_temps() {
    for (auto& kv : g_tempBySrc) {
        std::error_code ec;
        fs::remove(kv.second, ec);
    }
    g_tempBySrc.clear();
}

static std::string materialize_temp(const std::string& src, const std::vector<uint8_t>& plain) {
    auto it = g_tempBySrc.find(src);
    if (it != g_tempBySrc.end()) {
        std::error_code ec;
        fs::remove(it->second, ec);
    }
    char tmpDir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmpDir);
    if (n == 0 || n >= MAX_PATH) return {};
    char name[MAX_PATH];
    /* Generic prefix — avoid advertising "eye" in Temp. */
    if (GetTempFileNameA(tmpDir, "tmp", 0, name) == 0) return {};
    std::string ext = lower_ext(src);
    std::string out = std::string(name) + (ext.empty() ? ".bin" : ext);
    DeleteFileA(name);
    if (!write_raw(out, plain.data(), plain.size())) return {};
    SetFileAttributesA(out.c_str(), FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN);
    g_tempBySrc[src] = out;
    return out;
}

static std::vector<uint8_t> decrypt_blob(const std::vector<uint8_t>& b, const std::string& path) {
    if (b.size() < 20 || !g_key_ok) return {};
    uint8_t key[32], file_key[32];
    reveal_key(key);
    const uint8_t* nonce = b.data() + 8;
    const uint8_t* ct = b.data() + 20;
    std::vector<uint8_t> plain;
    if (g_secure) {
        if (!secure_fp_verify(0)) { wipe(key, 32); return {}; }
        if (b.size() < 20 + 16) { wipe(key, 32); return {}; }
        size_t n = b.size() - 20 - 16;
        uint8_t expect[16];
        auth_tag(key, nonce, ct, n, expect);
        uint8_t diff = 0;
        for (int i = 0; i < 16; ++i) diff |= expect[i] ^ b.data()[20 + n + i];
        if (diff) {
            wipe(key, 32);
            engine_log(("[content] integrity check failed: " + path).c_str());
            return {};
        }
        uint8_t n2[12];
        std::memcpy(n2, nonce, 12);
        n2[11] ^= 0xA7;
        std::vector<uint8_t> inner(n);
        if (n) chacha20_xor(key, n2, ct, inner.data(), n, 1);
        derive_file_key(key, norm_rel(path), file_key);
        plain.resize(n);
        if (n) chacha20_xor(file_key, nonce, inner.data(), plain.data(), n, 1);
        wipe(file_key, 32);
        wipe(key, 32);
        return plain;
    }
    if (g_auth) {
        if (b.size() < 20 + 16) { wipe(key, 32); return {}; }
        size_t n = b.size() - 20 - 16;
        if (!aead_decrypt(key, nonce, ct, n, b.data() + 20 + n, plain)) {
            wipe(key, 32);
            engine_log(("[content] integrity check failed: " + path).c_str());
            return {};
        }
    } else if (b.size() >= 20 + 16) {
        size_t n = b.size() - 20 - 16;
        if (!aead_decrypt(key, nonce, ct, n, b.data() + 20 + n, plain)) {
            size_t n2 = b.size() - 20;
            plain.resize(n2);
            if (n2) chacha20_xor(key, nonce, ct, plain.data(), n2, 1);
        }
    } else {
        size_t n = b.size() - 20;
        plain.resize(n);
        if (n) chacha20_xor(key, nonce, ct, plain.data(), n, 1);
    }
    wipe(key, 32);
    return plain;
}

} // namespace content_crypt

extern "C" void engine_content_key_clear() {
    using namespace content_crypt;
    wipe(g_key_enc, 32);
    wipe(g_key_mask, 32);
    wipe(g_fp_expect, 16);
    g_key_ok = 0;
    g_auth = 0;
    g_secure = 0;
    g_fp_ok = 0;
    g_fp_dll_path.clear();
    g_fp_exe_path.clear();
    clear_temps();
}

extern "C" void engine_content_set_protect(int level) {
    using namespace content_crypt;
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    g_protect = level;
    g_auth = (level >= 2) ? 1 : 0;
    g_secure = (level >= 3) ? 1 : 0;
}

extern "C" int engine_content_key_loaded() {
    return content_crypt::g_key_ok;
}

extern "C" int engine_content_key_load(const char* path) {
    using namespace content_crypt;
    engine_content_key_clear();
    if (!path || !path[0]) return 0;
    auto b = read_raw(path);
    if (b.empty()) return 0;
    if (load_key_from_exe_bytes(b)) return 1;
    /* Legacy loose content.key */
    if (b.size() == 8 + 32 && std::memcmp(b.data(), kLegacyKeyMagic, 8) == 0) {
        store_key(b.data() + 8);
        return 1;
    }
    return 0;
}

extern "C" int engine_content_key_generate() {
    using namespace content_crypt;
    engine_content_key_clear();
    uint8_t key[32];
    if (!fill_random(key, 32)) return 0;
    store_key(key);
    wipe(key, 32);
    return 1;
}

extern "C" int engine_content_key_create(const char* path) {
    using namespace content_crypt;
    if (!path || !path[0]) return 0;
    if (!engine_content_key_generate()) return 0;
    uint8_t key[32];
    reveal_key(key);
    std::vector<uint8_t> out;
    out.insert(out.end(), kLegacyKeyMagic, kLegacyKeyMagic + 8);
    out.insert(out.end(), key, key + 32);
    wipe(key, 32);
    return write_raw(path, out.data(), out.size());
}

extern "C" int engine_content_embed_key_ex(const char* exe_path, const char* dll_path);

extern "C" int engine_content_embed_key(const char* exe_path) {
    return engine_content_embed_key_ex(exe_path, nullptr);
}

extern "C" int engine_content_embed_key_ex(const char* exe_path, const char* dll_path) {
    using namespace content_crypt;
    if (!exe_path || !exe_path[0] || !g_key_ok) return 0;
    auto b = read_raw(exe_path);
    if (b.empty()) return 0;
    strip_embed_trailer(b);

    uint8_t key[32], mix[32], mask[32], wrapped[32], block[kEmbedSize];
    reveal_key(key);
    host_mix(mix);
    if (!fill_random(mask, 32)) { wipe(key, 32); wipe(mix, 32); return 0; }

    int tier = 1;
    if (g_protect >= 2 && dll_path && dll_path[0]) tier = g_protect >= 3 ? 3 : 2;
    if (tier >= 2) {
        if (tier >= 3) {
            uint8_t r[16], s1[16], s2[16];
            if (!fill_random(r, 16)) { wipe(key, 32); wipe(mix, 32); wipe(mask, 32); return 0; }
            for (int i = 0; i < 16; ++i) {
                s2[i] = r[i];
                s1[i] = key[16 + i] ^ mix[16 + i] ^ r[i];
            }
            wipe(r, 16);
            if (!patch_ksplit_file(dll_path, g_eyescire_ksplit, s1) ||
                !patch_ksplit_file(dll_path, g_eyescire_ksplit2, s2)) {
                wipe(s1, 16); wipe(s2, 16); wipe(key, 32); wipe(mix, 32); wipe(mask, 32);
                return 0;
            }
            wipe(s1, 16); wipe(s2, 16);
        } else {
            uint8_t half[16];
            for (int i = 0; i < 16; ++i) half[i] = key[16 + i] ^ mix[16 + i];
            if (!patch_ksplit_file(dll_path, g_eyescire_ksplit, half)) {
                wipe(half, 16); wipe(key, 32); wipe(mix, 32); wipe(mask, 32);
                return 0;
            }
            wipe(half, 16);
        }
        for (int i = 0; i < 16; ++i) wrapped[i] = key[i] ^ mask[i] ^ mix[i];
        if (tier >= 3) {
            /* Keyed seal of fingerprint — raw H is never stored in the trailer. */
            uint8_t fp[16], seal[16];
            if (!fingerprint_secure_pair(exe_path, dll_path, b.size(), fp)) {
                wipe(key, 32); wipe(mix, 32); wipe(mask, 32); wipe(wrapped, 32);
                return 0;
            }
            keyed_fp_seal(fp, seal);
            wipe(fp, 16);
            for (int i = 0; i < 16; ++i) wrapped[16 + i] = seal[i] ^ mask[16 + i];
            wipe(seal, 16);
        } else if (!fill_random(wrapped + 16, 16)) {
            wipe(key, 32); wipe(mix, 32); wipe(mask, 32); return 0;
        }
    } else {
        for (int i = 0; i < 32; ++i) wrapped[i] = key[i] ^ mask[i] ^ mix[i];
        tier = 1;
    }
    wipe(key, 32);
    wipe(mix, 32);
    std::memcpy(block, mask, 32);
    std::memcpy(block + 32, wrapped, 32);
    wipe(mask, 32);
    wipe(wrapped, 32);
    uint32_t crc = crc32_bytes(block, 64);
    uint32_t stamp = kEmbedStamp;
    if (tier == 2) stamp = kEmbedStampStrong;
    if (tier == 3) stamp = kEmbedStampSecure;
    put_u32(block + 64, crc);
    put_u32(block + 68, stamp ^ crc);
    b.insert(b.end(), block, block + kEmbedSize);
    wipe(block, kEmbedSize);
    return write_raw(exe_path, b.data(), b.size());
}

extern "C" int engine_content_key_load_self() {
    using namespace content_crypt;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    auto b = read_raw(path);
    engine_content_key_clear();
    return load_key_from_exe_bytes(b);
}

extern "C" int engine_content_should_encrypt(const char* path) {
    if (!path) return 0;
    return content_crypt::path_is_payload(path);
}

extern "C" int engine_content_encrypt_file(const char* src, const char* dst) {
    using namespace content_crypt;
    if (!src || !dst || !g_key_ok) return 0;
    auto plain = read_raw(src);
    if (plain.empty() && !fs::exists(src)) return 0;
    if (looks_encrypted(plain, dst) || looks_encrypted(plain, src)) {
        std::error_code ec;
        fs::path d(dst);
        if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return ec ? 0 : 1;
    }
    uint8_t key[32], nonce[12], tag[8], mac[16], file_key[32];
    reveal_key(key);
    if (!fill_random(nonce, 12)) { wipe(key, 32); return 0; }
    std::string base = norm_rel(dst);
    file_tag(key, base, tag);
    std::vector<uint8_t> cipher;
    int use_auth = (g_protect >= 2 || g_auth) ? 1 : 0;
    int use_secure = (g_protect >= 3 || g_secure) ? 1 : 0;
    if (use_secure) {
        /* Per-file key + dual ChaCha + master auth tag. */
        derive_file_key(key, base, file_key);
        std::vector<uint8_t> inner(plain.size());
        if (!plain.empty()) chacha20_xor(file_key, nonce, plain.data(), inner.data(), plain.size(), 1);
        wipe(file_key, 32);
        uint8_t n2[12];
        std::memcpy(n2, nonce, 12);
        n2[11] ^= 0xA7;
        cipher.resize(inner.size());
        if (!inner.empty()) chacha20_xor(key, n2, inner.data(), cipher.data(), inner.size(), 1);
        auth_tag(key, nonce, cipher.data(), cipher.size(), mac);
        use_auth = 1;
    } else if (use_auth) {
        aead_encrypt(key, nonce, plain.data(), plain.size(), cipher, mac);
    } else {
        cipher.resize(plain.size());
        if (!plain.empty()) chacha20_xor(key, nonce, plain.data(), cipher.data(), plain.size(), 1);
    }
    wipe(key, 32);
    wipe(file_key, 32);
    std::vector<uint8_t> out;
    out.reserve(8 + 12 + cipher.size() + 16);
    out.insert(out.end(), tag, tag + 8);
    out.insert(out.end(), nonce, nonce + 12);
    out.insert(out.end(), cipher.begin(), cipher.end());
    if (use_auth) out.insert(out.end(), mac, mac + 16);
    return write_raw(dst, out.data(), out.size());
}

extern "C" int engine_content_ship_file(const char* src, const char* dst, int encrypt) {
    using namespace content_crypt;
    if (!src || !dst) return 0;
    if (encrypt && g_key_ok && path_is_payload(dst ? dst : src))
        return engine_content_encrypt_file(src, dst);
    std::error_code ec;
    fs::path d(dst);
    if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return ec ? 0 : 1;
}

extern "C" int engine_content_ship_tree(const char* src, const char* dst, int encrypt) {
    using namespace content_crypt;
    if (!src || !dst) return 0;
    std::error_code ec;
    if (!fs::exists(src, ec)) return 0;
    fs::create_directories(dst, ec);
    for (fs::recursive_directory_iterator it(src, ec), end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::path& p = it->path();
        std::error_code sec;
        if (!it->is_regular_file(sec)) continue;
        fs::path rel = fs::relative(p, src, ec);
        if (ec) { ec.clear(); continue; }
        std::string out = (fs::path(dst) / rel).string();
        if (!engine_content_ship_file(p.string().c_str(), out.c_str(), encrypt))
            return 0;
    }
    return 1;
}

extern "C" int engine_content_ship_extensions(const char* src, const char* dst, int encrypt) {
    using namespace content_crypt;
    if (!src || !dst) return 0;
    std::error_code ec;
    if (!fs::is_directory(src, ec)) return 0;
    fs::create_directories(dst, ec);
    for (auto& entry : fs::directory_iterator(src, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_directory()) continue;
        std::string id = entry.path().filename().string();
        std::string outDir = (fs::path(dst) / id).string();
        fs::create_directories(outDir, ec);
        for (auto& fent : fs::directory_iterator(entry.path(), ec)) {
            if (ec) { ec.clear(); continue; }
            if (!fent.is_regular_file()) continue;
            std::string name = fent.path().filename().string();
            if (!path_is_ext_ship(name)) continue;
            std::string out = (fs::path(outDir) / name).string();
            if (!engine_content_ship_file(fent.path().string().c_str(), out.c_str(), encrypt))
                return 0;
        }
    }
    return 1;
}

static std::vector<uint8_t> content_read_bytes(const std::string& path) {
    using namespace content_crypt;
    auto b = read_raw(path);
    if (!looks_encrypted(b, path)) return b;
    if (!g_key_ok) {
        engine_log(("[content] encrypted file needs embedded key: " + path).c_str());
        return {};
    }
    return decrypt_blob(b, path);
}

static std::string content_resolve_path(const std::string& path) {
    using namespace content_crypt;
    auto b = read_raw(path);
    if (!looks_encrypted(b, path)) return path;
    if (!g_key_ok) {
        engine_log(("[content] encrypted file needs embedded key: " + path).c_str());
        return {};
    }
    auto plain = decrypt_blob(b, path);
    return materialize_temp(path, plain);
}
