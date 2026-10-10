/* Overlapping effects. Music stays on its own path. A one-shot can sit at a
   point, or an entity can carry a sound that follows it: quieter with
   distance, and shifted left or right. Wav and mp3 are decoded to PCM. */

#include <xaudio2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
#include <memory>

static int g_sfxLive = 0;

struct SfxCallback : IXAudio2VoiceCallback {
    std::atomic<int>* done = nullptr;
    int* looping = nullptr;
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override {
        if (looping && *looping) return;
        if (done) done->store(1);
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {
        if (done) done->store(1);
    }
};

struct PcmClip {
    std::vector<BYTE> fmt;
    std::vector<BYTE> pcm;
    WAVEFORMATEX* wf() { return fmt.size() >= sizeof(WAVEFORMATEX) ? (WAVEFORMATEX*)fmt.data() : nullptr; }
};

struct SfxSlot {
    IXAudio2SourceVoice* voice = nullptr;
    SfxCallback cb;
    std::atomic<int> done{0};
    int used = 0;
    int loop = 0;
    int spatial = 0;
    int follow = 0;
    int entity = -1;
    float x = 0, y = 0, z = 0;
    int range = 800;
    std::string path;
    PcmClip* clip = nullptr;
};

static const int kSfxMax = 16;
static SfxSlot g_sfxSlot[kSfxMax];
static IXAudio2* g_xa = nullptr;
static IXAudio2MasteringVoice* g_xaMaster = nullptr;
static int g_xaReady = 0;
static int g_xaFailed = 0;
static int g_mfReady = 0;
static std::unordered_map<std::string, std::unique_ptr<PcmClip>> g_pcmCache;
static std::unordered_set<std::string> g_sfxLogged;

static void sfx_log_once(const std::string& msg) {
    if (!g_sfxLogged.insert(msg).second) return;
    log_push(msg);
}

static std::wstring sfx_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

static HRESULT sfx_xaudio_create(IXAudio2** out) {
    static HMODULE dll = nullptr;
    if (!dll) dll = LoadLibraryExA("xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return E_FAIL;
    using Fn = HRESULT (__stdcall *)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR, DWORD);
    auto fn = (Fn)GetProcAddress(dll, "XAudio2CreateWithVersionInfo");
    if (!fn) return E_FAIL;
    return fn(out, 0, XAUDIO2_DEFAULT_PROCESSOR, 0x0A000006);
}

static int sfx_engine_ok() {
    if (g_xaReady) return 1;
    if (g_xaFailed) return 0;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        g_xaFailed = 1;
        sfx_log_once("[sfx] audio device failed to start");
        return 0;
    }
    hr = sfx_xaudio_create(&g_xa);
    if (FAILED(hr) || !g_xa) {
        g_xaFailed = 1;
        sfx_log_once("[sfx] audio device failed to start");
        return 0;
    }
    hr = g_xa->CreateMasteringVoice(&g_xaMaster);
    if (FAILED(hr) || !g_xaMaster) {
        g_xa->Release();
        g_xa = nullptr;
        g_xaFailed = 1;
        sfx_log_once("[sfx] audio device failed to start");
        return 0;
    }
    for (int i = 0; i < kSfxMax; ++i) {
        g_sfxSlot[i].cb.done = &g_sfxSlot[i].done;
        g_sfxSlot[i].cb.looping = &g_sfxSlot[i].loop;
    }
    g_xaReady = 1;
    return 1;
}

static PcmClip* sfx_decode(const std::string& path) {
    auto found = g_pcmCache.find(path);
    if (found != g_pcmCache.end()) return found->second.get();
    if (!g_mfReady) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
            sfx_log_once("[sfx] could not open sound files");
            return nullptr;
        }
        g_mfReady = 1;
    }
    std::wstring w = sfx_wide(path);
    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(w.c_str(), nullptr, &reader);
    if (FAILED(hr) || !reader) return nullptr;

    IMFMediaType* outType = nullptr;
    hr = MFCreateMediaType(&outType);
    if (FAILED(hr) || !outType) { reader->Release(); return nullptr; }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outType);
    outType->Release();
    if (FAILED(hr)) { reader->Release(); return nullptr; }

    IMFMediaType* got = nullptr;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got);
    WAVEFORMATEX* wf = nullptr;
    UINT32 wfSize = 0;
    if (!got || FAILED(MFCreateWaveFormatExFromMFMediaType(got, &wf, &wfSize)) || !wf) {
        if (got) got->Release();
        reader->Release();
        return nullptr;
    }
    got->Release();

    PcmClip clip;
    clip.fmt.assign((BYTE*)wf, (BYTE*)wf + wfSize);
    CoTaskMemFree(wf);

    for (;;) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (sample) sample->Release();
            break;
        }
        if (!sample) continue;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &len)) && data && len) {
                clip.pcm.insert(clip.pcm.end(), data, data + len);
                buffer->Unlock();
            }
            buffer->Release();
        }
        sample->Release();
    }
    reader->Release();
    if (clip.pcm.empty() || !clip.wf()) return nullptr;
    auto owned = std::make_unique<PcmClip>(std::move(clip));
    PcmClip* ptr = owned.get();
    g_pcmCache.emplace(path, std::move(owned));
    return ptr;
}

static void sfx_voice_stop(SfxSlot& s) {
    if (s.voice) {
        s.voice->Stop(0);
        s.voice->FlushSourceBuffers();
        s.voice->DestroyVoice();
        s.voice = nullptr;
    }
    s.used = 0;
    s.loop = 0;
    s.follow = 0;
    s.spatial = 0;
    s.entity = -1;
    s.clip = nullptr;
    s.path.clear();
    s.done.store(0);
}

static void sfx_stop_all() {
    for (int i = 0; i < kSfxMax; ++i) sfx_voice_stop(g_sfxSlot[i]);
}

static void sfx_shutdown() {
    sfx_stop_all();
    g_pcmCache.clear();
    if (g_xaMaster) { g_xaMaster->DestroyVoice(); g_xaMaster = nullptr; }
    if (g_xa) { g_xa->Release(); g_xa = nullptr; }
    if (g_mfReady) { MFShutdown(); g_mfReady = 0; }
    g_xaReady = 0;
}

struct SfxEar { float x, y, z, yaw; int mode; };

static SfxEar sfx_ear() {
    SfxEar L;
    L.x = g.width * 0.5f;
    L.y = g.height * 0.5f;
    L.z = 0;
    L.yaw = 0;
    L.mode = g.render3d ? 1 : 0;
    for (int s = 0; s < (int)g.entities.size(); ++s) {
        const Entity& e = g.entities[s];
        if (!e.active || !e.isCamera) continue;
        L.x = e.x + e.w * 0.5f;
        L.y = e.y + e.h * 0.5f;
        L.z = (float)e.z;
        int yaw = e.rotY;
        if (yaw > 180) yaw -= 360;
        L.yaw = (float)yaw;
        if (e.text == "look") {
            L.mode = 2;
            int pitch = e.rotX;
            if (pitch > 180) pitch -= 360;
            float pr = (float)pitch * 0.0174532925f;
            if (pr > 1.55f) pr = 1.55f;
            if (pr < -1.55f) pr = -1.55f;
            float yr = L.yaw * 0.0174532925f;
            float lx = std::cos(pr) * std::cos(yr);
            float ly = -std::sin(pr);
            float lz = std::cos(pr) * std::sin(yr);
            L.x += lx * 42.f;
            L.y += ly * 42.f;
            L.z += lz * 42.f;
        }
        break;
    }
    return L;
}

static void sfx_apply(SfxSlot& s, const SfxEar& L) {
    if (!s.voice) return;
    float master = g.sfxVolume / 1000.f;
    float atten = 1.f;
    float pan = 0.f;
    if (s.spatial) {
        float dx = s.x - L.x;
        float dy = s.y - L.y;
        float dz = s.z - L.z;
        float dist;
        float side;
        if (L.mode == 0) {
            dist = std::sqrt(dx * dx + dy * dy);
            side = dx;
        } else if (L.mode == 2) {
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float yr = L.yaw * 0.0174532925f;
            side = dx * std::sin(yr) + dz * (-std::cos(yr));
        } else {
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            side = dx;
        }
        float range = (float)s.range;
        if (range < 1.f) range = 1.f;
        atten = 1.f - dist / range;
        if (atten < 0.f) atten = 0.f;
        pan = side / (range * 0.5f);
        if (pan < -1.f) pan = -1.f;
        if (pan > 1.f) pan = 1.f;
    }
    float gain = master * atten;
    float angle = (pan + 1.f) * 0.78539816f;
    float left = std::cos(angle) * gain;
    float right = std::sin(angle) * gain;
    int nch = 2;
    if (s.clip && s.clip->wf() && s.clip->wf()->nChannels > 0)
        nch = s.clip->wf()->nChannels;
    if (nch > 8) nch = 8;
    float ch[8];
    if (nch == 1) {
        ch[0] = gain;
    } else {
        ch[0] = left;
        ch[1] = right;
        for (int i = 2; i < nch; ++i) ch[i] = gain;
    }
    s.voice->SetChannelVolumes((UINT32)nch, ch);
}

static int sfx_acquire() {
    for (int i = 0; i < kSfxMax; ++i) if (!g_sfxSlot[i].used) return i;
    for (int i = 0; i < kSfxMax; ++i) if (g_sfxSlot[i].used && !g_sfxSlot[i].loop) return i;
    return -1;
}

static int sfx_start(const std::string& resolved, float x, float y, float z,
                     int range, int loop, int spatial, int follow, int entity) {
    if (!sfx_engine_ok()) return 0;
    PcmClip* clip = sfx_decode(resolved);
    if (!clip || !clip->wf()) {
        sfx_log_once(std::string("[sfx] could not play: ") + resolved);
        return 0;
    }
    int idx = sfx_acquire();
    if (idx < 0) return 0;
    SfxSlot& s = g_sfxSlot[idx];
    sfx_voice_stop(s);
    HRESULT hr = g_xa->CreateSourceVoice(&s.voice, clip->wf(), 0, 1.0f, &s.cb);
    if (FAILED(hr) || !s.voice) {
        s.loop = 0;
        sfx_log_once(std::string("[sfx] could not play: ") + resolved);
        return 0;
    }
    s.loop = loop ? 1 : 0;
    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = (UINT32)clip->pcm.size();
    buf.pAudioData = clip->pcm.data();
    buf.Flags = XAUDIO2_END_OF_STREAM;
    if (s.loop) buf.LoopCount = XAUDIO2_LOOP_INFINITE;
    hr = s.voice->SubmitSourceBuffer(&buf);
    if (FAILED(hr)) {
        sfx_voice_stop(s);
        sfx_log_once(std::string("[sfx] could not play: ") + resolved);
        return 0;
    }
    s.used = 1;
    s.spatial = spatial ? 1 : 0;
    s.follow = follow ? 1 : 0;
    s.entity = entity;
    s.x = x; s.y = y; s.z = z;
    s.range = range > 0 ? range : 800;
    s.path = resolved;
    s.clip = clip;
    s.done.store(0);
    sfx_apply(s, sfx_ear());
    s.voice->Start(0);
    return 1;
}

static std::string sfx_resolve(const char* path) {
    if (!path || !*path) return "";
    std::string resolved = asset_resolve(path);
    std::error_code ec;
    if (!fs::exists(resolved, ec)) {
        std::string fallback = std::string(engine_exe_dir()) + "\\" + path;
        if (fs::exists(fallback, ec)) resolved = fallback;
        else {
            sfx_log_once(std::string("[sfx] missing: ") + path);
            return "";
        }
    }
    return resolved;
}

static void sfx_place_entity(const Entity& e, float& x, float& y, float& z) {
    x = e.x + e.w * 0.5f;
    y = e.y + e.h * 0.5f;
    z = e.z + e.depth * 0.5f;
}

static void sfx_restart_scene() {
    sfx_stop_all();
    if (!g_sfxLive) return;
    for (int i = 0; i < (int)g.entities.size(); ++i) {
        const Entity& e = g.entities[i];
        if (!e.active || e.sound.empty()) continue;
        std::string path = sfx_resolve(e.sound.c_str());
        if (path.empty()) continue;
        float x, y, z;
        sfx_place_entity(e, x, y, z);
        sfx_start(path, x, y, z, e.soundRange, e.soundLoop, 1, 1, i);
    }
}

static void sfx_tick() {
    if (!g_xaReady) return;
    SfxEar ear = sfx_ear();
    for (int i = 0; i < kSfxMax; ++i) {
        SfxSlot& s = g_sfxSlot[i];
        if (!s.used) continue;
        if (s.done.load()) { sfx_voice_stop(s); continue; }
        if (s.follow) {
            if (s.entity < 0 || s.entity >= (int)g.entities.size() || !g.entities[s.entity].active
                || g.entities[s.entity].sound.empty()) {
                sfx_voice_stop(s);
                continue;
            }
            sfx_place_entity(g.entities[s.entity], s.x, s.y, s.z);
            s.range = g.entities[s.entity].soundRange > 0 ? g.entities[s.entity].soundRange : 800;
        }
        sfx_apply(s, ear);
    }
}

extern "C" int engine_sfx_play(const char* path) {
    std::string resolved = sfx_resolve(path);
    if (resolved.empty()) return 0;
    return sfx_start(resolved, 0, 0, 0, 800, 0, 0, 0, -1);
}

extern "C" int engine_sfx_play_at(const char* path, float x, float y, float z) {
    std::string resolved = sfx_resolve(path);
    if (resolved.empty()) return 0;
    return sfx_start(resolved, x, y, z, 800, 0, 1, 0, -1);
}

static void sfx_on_scene_loaded() {
    if (g_sfxLive) sfx_restart_scene();
}

extern "C" void engine_sfx_set_live(int on) {
    int want = on ? 1 : 0;
    if (g_sfxLive == want) {
        if (want) sfx_restart_scene();
        return;
    }
    g_sfxLive = want;
    if (want) sfx_restart_scene();
    else sfx_stop_all();
}
