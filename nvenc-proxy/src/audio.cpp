#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <intrin.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio.h"
#include "common.h"
#include "output.h"

// ---- sample formats ---------------------------------------------------------------------------

enum class SampleType : uint8_t { I16, I24, I32, F32, F64, I32in16, I32in24, Unknown };

static int bytes_of(SampleType t) {
    switch (t) {
    case SampleType::I16: return 2;
    case SampleType::I24: return 3;
    case SampleType::F64: return 8;
    default:              return 4;
    }
}

static inline float read_sample(const uint8_t* p, SampleType t) {
    switch (t) {
    case SampleType::I16:     return *(const int16_t*)p * (1.0f / 32768.0f);
    case SampleType::I24:     return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) * (1.0f / 2147483648.0f);
    case SampleType::I32:     return *(const int32_t*)p * (1.0f / 2147483648.0f);
    case SampleType::F32:     return *(const float*)p;
    case SampleType::F64:     return (float)*(const double*)p;
    case SampleType::I32in16: return *(const int32_t*)p * (1.0f / 32768.0f);
    case SampleType::I32in24: return *(const int32_t*)p * (1.0f / 8388608.0f);
    default:                  return 0.0f;
    }
}

// ---- lock-free single-producer ring (game audio thread -> encoder thread) ---------------------

static const uint64_t kRingFrames = 1 << 17;          // ~2.7 s of stereo at 48 kHz
static const uint64_t kMeta = 1024;

struct Meta { int64_t when; uint32_t frames; uint32_t rate; };

static float                 g_ring[kRingFrames * 2];
static Meta                  g_meta[kMeta];
static std::atomic<uint64_t> g_wr{ 0 }, g_rd{ 0 }, g_mwr{ 0 }, g_mrd{ 0 };
static std::atomic<uint64_t> g_dropped{ 0 };

// Only one stream feeds the ring; another may take over after this one is silent for 1 s.
static std::atomic<uintptr_t> g_source{ 0 };
static std::atomic<int64_t>   g_sourceSeen{ 0 };
static std::atomic<int>       g_sourceKind{ 0 };       // 1 ASIO, 2 WASAPI (for the log)

static bool claim_source(uintptr_t id, int kind, int64_t now) {
    uintptr_t cur = g_source.load(std::memory_order_relaxed);
    if (cur != id) {
        if (cur && now - g_sourceSeen.load(std::memory_order_relaxed) < 10000000) return false;
        g_source.store(id, std::memory_order_relaxed);
        g_sourceKind.store(kind, std::memory_order_relaxed);
    }
    g_sourceSeen.store(now, std::memory_order_relaxed);
    return true;
}

// Runs on the game's audio thread: bounded work, no locks, no allocation, never blocks.
template <class Get>
static void produce(uint32_t frames, uint32_t rate, int64_t when, Get&& get) {
    uint64_t wr = g_wr.load(std::memory_order_relaxed), rd = g_rd.load(std::memory_order_acquire);
    uint64_t mw = g_mwr.load(std::memory_order_relaxed), mr = g_mrd.load(std::memory_order_acquire);
    if (wr + frames - rd > kRingFrames || mw - mr >= kMeta) { g_dropped.fetch_add(1, std::memory_order_relaxed); return; }
    for (uint32_t i = 0; i < frames; ++i) {
        size_t k = (size_t)((wr + i) % kRingFrames) * 2;
        g_ring[k] = get(i, 0);
        g_ring[k + 1] = get(i, 1);
    }
    g_meta[mw % kMeta] = { when, frames, rate };
    g_wr.store(wr + frames, std::memory_order_release);
    g_mwr.store(mw + 1, std::memory_order_release);
}

// Overhead accounting so the log can show the tap is negligible next to the game's own callback.
struct TapStats {
    std::atomic<uint64_t> calls{ 0 }, tapTicks{ 0 }, tapMax{ 0 }, gameTicks{ 0 };
    void add(uint64_t game, uint64_t tap) {
        calls.fetch_add(1, std::memory_order_relaxed);
        gameTicks.fetch_add(game, std::memory_order_relaxed);
        tapTicks.fetch_add(tap, std::memory_order_relaxed);
        uint64_t m = tapMax.load(std::memory_order_relaxed);
        while (tap > m && !tapMax.compare_exchange_weak(m, tap, std::memory_order_relaxed)) {}
    }
};
static TapStats g_asioStats, g_wasapiStats;

static inline uint64_t ticks() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart; }

// ---- WASAPI -----------------------------------------------------------------------------------

enum { kAcInitialize = 3, kAcGetService = 14, kRcGetBuffer = 3, kRcReleaseBuffer = 4 };

typedef HRESULT (STDMETHODCALLTYPE* PFN_AcInitialize)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
typedef HRESULT (STDMETHODCALLTYPE* PFN_AcGetService)(IAudioClient*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_RcGetBuffer)(IAudioRenderClient*, UINT32, BYTE**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_RcReleaseBuffer)(IAudioRenderClient*, UINT32, DWORD);

struct StreamFmt { SampleType type = SampleType::Unknown; int channels = 0, rate = 0, align = 0; };

struct RenderStream {
    std::atomic<IAudioRenderClient*> rc{ nullptr };
    IAudioClient* client = nullptr;
    StreamFmt     fmt;
    BYTE*         buf = nullptr;       // from the last GetBuffer (same thread as ReleaseBuffer)
};

static std::mutex    g_fmtMtx;
static std::unordered_map<IAudioClient*, StreamFmt> g_clientFmt;
static RenderStream  g_streams[16];
static std::atomic<int> g_streamCount{ 0 };

static bool parse_wfx(const WAVEFORMATEX* f, StreamFmt& out) {
    if (!f) return false;
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT, isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        auto x = (const WAVEFORMATEXTENSIBLE*)f;
        isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        isPcm = x->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    }
    SampleType t = SampleType::Unknown;
    if (isFloat && f->wBitsPerSample == 32) t = SampleType::F32;
    else if (isFloat && f->wBitsPerSample == 64) t = SampleType::F64;
    else if (isPcm && f->wBitsPerSample == 16) t = SampleType::I16;
    else if (isPcm && f->wBitsPerSample == 24) t = SampleType::I24;
    else if (isPcm && f->wBitsPerSample == 32) t = SampleType::I32;   // 24-in-32 is left-justified
    if (t == SampleType::Unknown || !f->nChannels) return false;
    out = { t, f->nChannels, (int)f->nSamplesPerSec, f->nBlockAlign };
    return true;
}

static RenderStream* find_stream(IAudioRenderClient* rc) {
    int n = g_streamCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) if (g_streams[i].rc.load(std::memory_order_relaxed) == rc) return &g_streams[i];
    return nullptr;
}

// A stream opened before our hooks never passed Initialize/GetService. Shared-mode streams (what
// spice2x forces) use the device mix format, so adopt it with that format.
static StreamFmt g_mixFmt;

static RenderStream* adopt_stream(IAudioRenderClient* rc) {
    if (g_mixFmt.type == SampleType::Unknown) return nullptr;
    std::lock_guard<std::mutex> lk(g_fmtMtx);
    if (RenderStream* s = find_stream(rc)) return s;
    int n = g_streamCount.load();
    if (n >= 16) return nullptr;
    g_streams[n].client = nullptr;
    g_streams[n].fmt = g_mixFmt;
    g_streams[n].rc.store(rc);
    g_streamCount.store(n + 1, std::memory_order_release);
    logf("WASAPI stream opened before our hooks: assuming the shared-mode mix format (%d ch, %d Hz)",
         g_mixFmt.channels, g_mixFmt.rate);
    return &g_streams[n];
}

static HRESULT STDMETHODCALLTYPE Hook_AcInitialize(IAudioClient* self, AUDCLNT_SHAREMODE mode, DWORD flags,
                                                   REFERENCE_TIME dur, REFERENCE_TIME per,
                                                   const WAVEFORMATEX* fmt, LPCGUID session) {
    HRESULT hr = ((PFN_AcInitialize)vt_orig(self, kAcInitialize))(self, mode, flags, dur, per, fmt, session);
    StreamFmt f;
    if (SUCCEEDED(hr) && parse_wfx(fmt, f)) {
        std::lock_guard<std::mutex> lk(g_fmtMtx);
        g_clientFmt[self] = f;
        logf("WASAPI stream: %s, %d ch, %d Hz, %d-bit %s", mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? "exclusive" : "shared",
             f.channels, f.rate, fmt->wBitsPerSample, f.type == SampleType::F32 || f.type == SampleType::F64 ? "float" : "int");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_AcGetService(IAudioClient* self, REFIID riid, void** ppv) {
    HRESULT hr = ((PFN_AcGetService)vt_orig(self, kAcGetService))(self, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && riid == __uuidof(IAudioRenderClient)) {
        std::lock_guard<std::mutex> lk(g_fmtMtx);
        auto it = g_clientFmt.find(self);
        int n = g_streamCount.load();
        if (it != g_clientFmt.end() && n < 16 && !find_stream((IAudioRenderClient*)*ppv)) {
            g_streams[n].client = self;
            g_streams[n].fmt = it->second;
            g_streams[n].rc.store((IAudioRenderClient*)*ppv);
            g_streamCount.store(n + 1, std::memory_order_release);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_RcGetBuffer(IAudioRenderClient* self, UINT32 frames, BYTE** data) {
    HRESULT hr = ((PFN_RcGetBuffer)vt_orig(self, kRcGetBuffer))(self, frames, data);
    if (SUCCEEDED(hr) && data) {
        RenderStream* s = find_stream(self);
        if (!s) s = adopt_stream(self);
        if (s) s->buf = *data;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_RcReleaseBuffer(IAudioRenderClient* self, UINT32 frames, DWORD flags) {
    auto orig = (PFN_RcReleaseBuffer)vt_orig(self, kRcReleaseBuffer);
    RenderStream* s = frames ? find_stream(self) : nullptr;
    if (s && s->buf && output_active()) {
        uint64_t t0 = ticks();
        int64_t now = qpc_100ns();
        if (claim_source((uintptr_t)self, 2, now)) {
            // Frames already queued ahead of this buffer play first.
            UINT32 pad = 0;
            if (s->client) s->client->GetCurrentPadding(&pad);
            const StreamFmt f = s->fmt;
            const BYTE* buf = s->buf;
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            int bps = bytes_of(f.type);
            int64_t when = now + (int64_t)pad * 10000000 / f.rate;
            produce(frames, (uint32_t)f.rate, when, [&](uint32_t i, int c) {
                if (silent) return 0.0f;
                int cc = c < f.channels ? c : 0;
                return read_sample(buf + (size_t)i * f.align + (size_t)cc * bps, f.type);
            });
            g_wasapiStats.add(0, ticks() - t0);
        }
    }
    return orig(self, frames, flags);
}

static void install_wasapi() {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))
        || FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))
        || FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))
        || FAILED(ac->GetMixFormat(&mix))
        || FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr))
        || FAILED(ac->GetService(IID_PPV_ARGS(&rc)))) {
        logf("WASAPI hook unavailable (no default render device?)");
    } else {
        parse_wfx(mix, g_mixFmt);
        vt_hook(ac, kAcInitialize, (void*)&Hook_AcInitialize);
        vt_hook(ac, kAcGetService, (void*)&Hook_AcGetService);
        vt_hook(rc, kRcGetBuffer, (void*)&Hook_RcGetBuffer);
        vt_hook(rc, kRcReleaseBuffer, (void*)&Hook_RcReleaseBuffer);
        logf("WASAPI render hooked");
    }
    if (mix) CoTaskMemFree(mix);
    if (rc) rc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
}

// ---- ASIO -------------------------------------------------------------------------------------

typedef long ASIOBool;
typedef long ASIOError;
struct ASIOBufferInfo { ASIOBool isInput; long channelNum; void* buffers[2]; };
struct ASIOChannelInfo { long channel; ASIOBool isInput; ASIOBool isActive; long channelGroup; long type; char name[32]; };
struct ASIOCallbacks {
    void  (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void  (*sampleRateDidChange)(double sRate);
    long  (*asioMessage)(long selector, long value, void* message, double* opt);
    void* (*bufferSwitchTimeInfo)(void* params, long doubleBufferIndex, ASIOBool directProcess);
};

enum { kAsioGetLatencies = 10, kAsioGetSampleRate = 13, kAsioGetChannelInfo = 18,
       kAsioCreateBuffers = 19, kAsioDisposeBuffers = 20 };

typedef ASIOError (*PFN_GetLatencies)(void*, long*, long*);
typedef ASIOError (*PFN_GetSampleRate)(void*, double*);
typedef ASIOError (*PFN_GetChannelInfo)(void*, ASIOChannelInfo*);
typedef ASIOError (*PFN_CreateBuffers)(void*, ASIOBufferInfo*, long, long, ASIOCallbacks*);
typedef ASIOError (*PFN_DisposeBuffers)(void*);

static ASIOCallbacks     g_gameCb;
static ASIOCallbacks     g_ourCb;
static void*             g_asioBuf[2][2];      // [channel][half]
static SampleType        g_asioType[2];
static int               g_asioCh;
static long              g_asioSize;
static std::atomic<double> g_asioRate{ 0 };
static int64_t           g_asioLatency;         // 100ns from bufferSwitch to the output
static std::atomic<bool> g_asioReady{ false };
static std::vector<CLSID> g_asioClsids;

static SampleType asio_type(long t) {
    switch (t) {
    case 16: return SampleType::I16;      // ASIOSTInt16LSB
    case 17: return SampleType::I24;      // ASIOSTInt24LSB
    case 18: return SampleType::I32;      // ASIOSTInt32LSB
    case 19: return SampleType::F32;      // ASIOSTFloat32LSB
    case 20: return SampleType::F64;      // ASIOSTFloat64LSB
    case 24: return SampleType::I32in16;  // ASIOSTInt32LSB16
    case 27: return SampleType::I32in24;  // ASIOSTInt32LSB24
    default: return SampleType::Unknown;
    }
}

static void asio_tap(long idx) {
    double rate = g_asioRate.load(std::memory_order_relaxed);
    if (!g_asioReady.load(std::memory_order_acquire) || rate <= 0 || !output_active()) return;
    int64_t now = qpc_100ns();
    if (!claim_source(1, 1, now)) return;
    const int ch = g_asioCh;
    const long half = idx & 1;
    produce((uint32_t)g_asioSize, (uint32_t)rate, now + g_asioLatency, [&](uint32_t i, int c) {
        int cc = c < ch ? c : 0;
        SampleType t = g_asioType[cc];
        return read_sample((const uint8_t*)g_asioBuf[cc][half] + (size_t)i * bytes_of(t), t);
    });
}

// The driver calls these instead of the game's callbacks. Each forwards first, unchanged, and only
// then copies the half-buffer the game just filled, so the game's deadline and data are untouched.
static void Our_BufferSwitch(long idx, ASIOBool direct) {
    uint64_t t0 = ticks();
    g_gameCb.bufferSwitch(idx, direct);
    uint64_t t1 = ticks();
    asio_tap(idx);
    g_asioStats.add(t1 - t0, ticks() - t1);
}

static void* Our_BufferSwitchTimeInfo(void* params, long idx, ASIOBool direct) {
    uint64_t t0 = ticks();
    void* r = g_gameCb.bufferSwitchTimeInfo(params, idx, direct);
    uint64_t t1 = ticks();
    asio_tap(idx);
    g_asioStats.add(t1 - t0, ticks() - t1);
    return r;
}

static void Our_SampleRateDidChange(double rate) {
    g_asioRate.store(rate);
    if (g_gameCb.sampleRateDidChange) g_gameCb.sampleRateDidChange(rate);
}

static long Our_AsioMessage(long sel, long value, void* msg, double* opt) {
    return g_gameCb.asioMessage ? g_gameCb.asioMessage(sel, value, msg, opt) : 0;
}

static ASIOError Hook_CreateBuffers(void* self, ASIOBufferInfo* infos, long num, long size, ASIOCallbacks* cb) {
    auto orig = (PFN_CreateBuffers)vt_orig(self, kAsioCreateBuffers);
    if (!cb) return orig(self, infos, num, size, cb);
    g_asioReady = false;
    g_gameCb = *cb;
    g_ourCb.bufferSwitch = cb->bufferSwitch ? &Our_BufferSwitch : nullptr;
    g_ourCb.sampleRateDidChange = &Our_SampleRateDidChange;
    g_ourCb.asioMessage = &Our_AsioMessage;
    g_ourCb.bufferSwitchTimeInfo = cb->bufferSwitchTimeInfo ? &Our_BufferSwitchTimeInfo : nullptr;
    ASIOError r = orig(self, infos, num, size, &g_ourCb);
    if (r != 0) return r;

    int n = 0;
    for (long i = 0; i < num && n < 2; ++i) {
        if (infos[i].isInput) continue;
        ASIOChannelInfo ci = {};
        ci.channel = infos[i].channelNum;
        ci.isInput = 0;
        ((PFN_GetChannelInfo)(*(void***)self)[kAsioGetChannelInfo])(self, &ci);
        g_asioType[n] = asio_type(ci.type);
        g_asioBuf[n][0] = infos[i].buffers[0];
        g_asioBuf[n][1] = infos[i].buffers[1];
        ++n;
    }
    double rate = 0;
    ((PFN_GetSampleRate)(*(void***)self)[kAsioGetSampleRate])(self, &rate);
    long inLat = 0, outLat = 0;
    ((PFN_GetLatencies)(*(void***)self)[kAsioGetLatencies])(self, &inLat, &outLat);
    g_asioCh = n;
    g_asioSize = size;
    g_asioRate = rate;
    g_asioLatency = rate > 0 ? (int64_t)(outLat * 10000000.0 / rate) : 0;
    bool ok = n > 0 && g_asioType[0] != SampleType::Unknown && (n < 2 || g_asioType[1] != SampleType::Unknown);
    logf("ASIO buffers: %ld frames @ %.0f Hz, %d output ch tapped (type %d), output latency %ld frames, %s",
         size, rate, n, n ? (int)g_asioType[0] : -1, outLat, ok ? "tap ready" : "unsupported sample type, not tapped");
    g_asioReady.store(ok, std::memory_order_release);
    return r;
}

static ASIOError Hook_DisposeBuffers(void* self) {
    g_asioReady = false;
    return ((PFN_DisposeBuffers)vt_orig(self, kAsioDisposeBuffers))(self);
}

static std::mutex          g_asioVtMtx;
static std::vector<void**> g_asioVtbls;

static void hook_asio_driver(void* drv) {
    // Pin the driver DLL so COM can't unload it (and our patch with it) between instances.
    HMODULE pin = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)*(void**)drv, &pin);
    vt_hook(drv, kAsioCreateBuffers, (void*)&Hook_CreateBuffers);
    vt_hook(drv, kAsioDisposeBuffers, (void*)&Hook_DisposeBuffers);
    std::lock_guard<std::mutex> lk(g_asioVtMtx);
    void** vt = *(void***)drv;
    if (std::find(g_asioVtbls.begin(), g_asioVtbls.end(), vt) == g_asioVtbls.end()) {
        g_asioVtbls.push_back(vt);
        logf("ASIO vtable %p in %s hooked", (void*)vt, module_name(vt).c_str());
    }
}

static bool is_asio(REFCLSID c) {
    for (auto& a : g_asioClsids) if (a == c) return true;
    return false;
}

typedef HRESULT (STDAPICALLTYPE* PFN_CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PFN_CoCreateInstance g_realCoCreate;

// Catches the game's own ASIO driver instance, whichever module creates it (under spice2x it is
// spice64.exe, whose hook sits in bm2dx.dll's import table).
static HRESULT STDAPICALLTYPE Hook_CoCreateInstance(REFCLSID c, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* ppv) {
    void* caller = _ReturnAddress();
    HRESULT hr = g_realCoCreate(c, outer, ctx, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && is_asio(c)) {
        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true)) logf("ASIO driver created by %s", module_name(caller).c_str());
        hook_asio_driver(*ppv);
    }
    return hr;
}

static void install_asio() {
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) == ERROR_SUCCESS) {
        wchar_t name[256];
        for (DWORD i = 0; RegEnumKeyW(root, i, name, 256) == ERROR_SUCCESS; ++i) {
            wchar_t clsid[64] = {}; DWORD sz = sizeof clsid;
            CLSID c;
            if (RegGetValueW(root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &sz) == ERROR_SUCCESS &&
                SUCCEEDED(CLSIDFromString(clsid, &c))) {
                g_asioClsids.push_back(c);
                // Constructing (not init()-ing) the driver object finds its vtable without touching
                // the hardware; the game's own instance shares it.
                void* drv = nullptr;
                if (g_cfg.asioProbe && SUCCEEDED(CoCreateInstance(c, nullptr, CLSCTX_INPROC_SERVER, c, &drv)) && drv) {
                    hook_asio_driver(drv);
                    ((IUnknown*)drv)->Release();
                    logf("ASIO driver '%ls' hooked", name);
                } else {
                    logf("ASIO driver '%ls' found (hooked when the game creates it)", name);
                }
            }
        }
        RegCloseKey(root);
    }
    HMODULE ole = GetModuleHandleW(L"combase.dll");
    if (!ole) ole = LoadLibraryW(L"ole32.dll");
    void* real = GetProcAddress(ole, "CoCreateInstance");
    if (!real) return;
    // In combase's code when its prologue allows it, otherwise in every module's import table.
    if (void* next = code_hook(real, (void*)&Hook_CoCreateInstance)) {
        g_realCoCreate = (PFN_CoCreateInstance)next;
        logf("CoCreateInstance hooked in %s", module_name(real).c_str());
    } else {
        g_realCoCreate = (PFN_CoCreateInstance)real;
        iat_hook_all(real, (void*)&Hook_CoCreateInstance, "CoCreateInstance");
    }
}

// Logged once if video has been streaming for a while without any audio reaching us.
static void audio_watchdog_report() {
    logf("audio watchdog: no game audio has reached the proxy yet");
    {
        std::lock_guard<std::mutex> lk(g_asioVtMtx);
        for (void** vt : g_asioVtbls)
            logf("  ASIO vtable %p: createBuffers slot -> %s (%s)", (void*)vt, module_name(vt[kAsioCreateBuffers]).c_str(),
                 vt[kAsioCreateBuffers] == (void*)&Hook_CreateBuffers ? "our hook" : "NOT ours");
    }
    logf("  WASAPI render streams seen: %d", g_streamCount.load());
    HMODULE mods[1024]; DWORD needed = 0;
    EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed);
    std::string list;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
        std::string n = module_name(mods[i]), l = n;
        for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
        static const char* keys[] = { "asio", "xonar", "nhasus", "audio", "sound", "dsound", "xaudio", "wasapi", "mmdev", "avrt", "ks", "u7" };
        for (const char* k : keys) if (l.find(k) != std::string::npos) { list += " " + n; break; }
    }
    logf("  audio-related modules:%s", list.c_str());
}

// ---- AAC encoder thread -----------------------------------------------------------------------

static int freq_index(int rate) { return rate == 44100 ? 4 : 3; }

static IMFTransform* make_aac(int rate) {
    IMFTransform* t = nullptr;
    if (FAILED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) return nullptr;
    IMFMediaType *in = nullptr, *out = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    int kbps = g_cfg.audioBitrateKbps;
    int bytes = kbps >= 192 ? 24000 : kbps >= 160 ? 20000 : kbps >= 128 ? 16000 : 12000;
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes);
    bool ok = SUCCEEDED(t->SetInputType(0, in, 0)) && SUCCEEDED(t->SetOutputType(0, out, 0));
    if (!ok) ok = SUCCEEDED(t->SetOutputType(0, out, 0)) && SUCCEEDED(t->SetInputType(0, in, 0));
    in->Release(); out->Release();
    if (!ok) { t->Release(); return nullptr; }
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    return t;
}

static void encode_chunk(IMFTransform* aac, const int16_t* pcm, int frames, int64_t pts, int rate) {
    IMFMediaBuffer* b = nullptr;
    if (FAILED(MFCreateMemoryBuffer(frames * 4, &b))) return;
    BYTE* p = nullptr;
    b->Lock(&p, nullptr, nullptr);
    memcpy(p, pcm, (size_t)frames * 4);
    b->Unlock();
    b->SetCurrentLength(frames * 4);
    IMFSample* s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(b);
    b->Release();
    s->SetSampleTime(pts);
    s->SetSampleDuration((int64_t)frames * 10000000 / rate);
    aac->ProcessInput(0, s, 0);
    s->Release();

    for (;;) {
        MFT_OUTPUT_STREAM_INFO si = {};
        aac->GetOutputStreamInfo(0, &si);
        IMFSample* o = nullptr; IMFMediaBuffer* ob = nullptr;
        MFCreateSample(&o);
        MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 8192, &ob);
        o->AddBuffer(ob); ob->Release();
        MFT_OUTPUT_DATA_BUFFER od = {}; od.pSample = o;
        DWORD status = 0;
        HRESULT hr = aac->ProcessOutput(0, 1, &od, &status);
        if (od.pEvents) od.pEvents->Release();
        if (FAILED(hr)) { o->Release(); break; }
        Item it;
        it.kind = ItemKind::Audio;
        it.session = live_session();
        LONGLONG t = pts;
        o->GetSampleTime(&t);
        it.pts = it.dts = it.wall = t;
        IMFMediaBuffer* cb = nullptr;
        if (SUCCEEDED(o->ConvertToContiguousBuffer(&cb))) {
            BYTE* d = nullptr; DWORD len = 0;
            if (SUCCEEDED(cb->Lock(&d, nullptr, &len))) { it.data.assign(d, d + len); cb->Unlock(); }
            cb->Release();
        }
        o->Release();
        if (!it.data.empty()) output_push(std::move(it));
    }
}

static void log_stats(const char* name, TapStats& s, double freq) {
    uint64_t n = s.calls.exchange(0);
    if (!n) return;
    double tap = s.tapTicks.exchange(0) * 1e6 / freq / n, game = s.gameTicks.exchange(0) * 1e6 / freq / n;
    double mx = s.tapMax.exchange(0) * 1e6 / freq;
    if (game > 0) logf("%s tap: %llu callbacks, game callback avg %.1f us, our copy avg %.2f us (max %.1f us)",
                       name, (unsigned long long)n, game, tap, mx);
    else          logf("%s tap: %llu buffers, our copy avg %.2f us (max %.1f us)", name, (unsigned long long)n, tap, mx);
}

static void audio_worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double freq = (double)f.QuadPart;
    // The harness sync test (flash + tick scheduled for the same instant) shows decoded audio
    // landing ~25 ms ahead of its video at both 44.1 and 48 kHz, so that is corrected here;
    // offset_ms is the user's adjustment on top.
    const int64_t kMeasuredSkew = 250000;
    const int64_t offset = kMeasuredSkew + (int64_t)g_cfg.audioOffsetMs * 10000;

    IMFTransform* aac = nullptr;
    int      aacRate = 0;
    std::vector<int16_t> pending;      // interleaved stereo at aacRate, not yet encoded
    std::vector<float>   blk;
    bool     anchored = false;
    int64_t  anchorPts = 0;
    uint64_t encoded = 0;              // frames encoded since the anchor
    int      lastKind = 0;
    uint64_t reanchors = 0;
    int64_t  nextStats = qpc_100ns() + 600000000;   // stats every 60 s

    for (;;) {
        int64_t nowT = qpc_100ns();
        if (nowT >= nextStats) {
            log_stats("ASIO", g_asioStats, freq);
            log_stats("WASAPI", g_wasapiStats, freq);
            uint64_t d = g_dropped.exchange(0);
            if (d || reanchors) logf("audio: %llu blocks dropped (ring full), %llu clock re-syncs",
                                     (unsigned long long)d, (unsigned long long)reanchors);
            reanchors = 0;
            nextStats = nowT + 600000000;
        }
        // Someone has been watching for 30 s and no game audio arrived: log what we can see.
        static int64_t activeSince = 0;
        static bool    watchdogDone = false;
        if (!output_active()) activeSince = 0;
        else if (!activeSince) activeSince = nowT;
        if (!watchdogDone && activeSince && g_sourceKind.load() == 0 && nowT - activeSince > 300000000) {
            audio_watchdog_report();
            watchdogDone = true;
        }
        int kind = g_sourceKind.load();
        if (kind != lastKind) { logf("audio source: %s", kind == 1 ? "ASIO" : "WASAPI"); lastKind = kind; }

        uint64_t mr = g_mrd.load(std::memory_order_relaxed);
        if (mr == g_mwr.load(std::memory_order_acquire)) { Sleep(2); continue; }
        Meta m = g_meta[mr % kMeta];
        uint64_t rd = g_rd.load(std::memory_order_relaxed);
        blk.resize((size_t)m.frames * 2);
        for (uint32_t i = 0; i < m.frames; ++i) {
            size_t k = (size_t)((rd + i) % kRingFrames) * 2;
            blk[i * 2] = g_ring[k];
            blk[i * 2 + 1] = g_ring[k + 1];
        }
        g_rd.store(rd + m.frames, std::memory_order_release);
        g_mrd.store(mr + 1, std::memory_order_release);
        if (!output_active() || !m.frames || !m.rate) { pending.clear(); anchored = false; continue; }

        int rate = (m.rate == 44100 || m.rate == 48000) ? (int)m.rate : 48000;
        if (rate != aacRate || !aac) {
            if (aac) aac->Release();
            aac = make_aac(rate);
            aacRate = rate;
            pending.clear();
            anchored = false;
            if (!aac) { logf("audio: AAC encoder unavailable for %d Hz", rate); continue; }
            AudioInfo ai;
            ai.present = true; ai.adts = false; ai.objectType = 2; ai.freqIndex = freq_index(rate); ai.channels = 2;
            live_update_audio(ai);
            logf("audio: AAC %d Hz stereo, %d kbps", rate, g_cfg.audioBitrateKbps);
        }

        // Linear resample only for device rates AAC can't take directly (e.g. 96 kHz).
        uint32_t outFrames = m.frames;
        if ((int)m.rate != rate) {
            outFrames = (uint32_t)((uint64_t)m.frames * rate / m.rate);
            std::vector<float> rs((size_t)outFrames * 2);
            for (uint32_t i = 0; i < outFrames; ++i) {
                double src = (double)i * m.rate / rate;
                uint32_t a = (uint32_t)src, b = std::min(a + 1, m.frames - 1);
                float fr = (float)(src - a);
                rs[i * 2] = blk[a * 2] + (blk[b * 2] - blk[a * 2]) * fr;
                rs[i * 2 + 1] = blk[a * 2 + 1] + (blk[b * 2 + 1] - blk[a * 2 + 1]) * fr;
            }
            blk.swap(rs);
        }

        // Timestamps follow the audio device's sample clock, re-synced to QPC only if it wanders.
        int64_t when = m.when + offset;
        uint64_t queued = pending.size() / 2;
        int64_t expected = anchorPts + (int64_t)((encoded + queued) * 10000000 / rate);
        if (!anchored || _abs64(when - expected) > 800000) {
            if (anchored) ++reanchors;
            anchorPts = when - (int64_t)(queued * 10000000 / rate);
            encoded = 0;
            anchored = true;
        }
        for (uint32_t i = 0; i < outFrames * 2; ++i) {
            float v = std::max(-1.0f, std::min(1.0f, blk[i]));
            pending.push_back((int16_t)(v * 32767.0f));
        }
        size_t used = 0;
        while (pending.size() - used >= 2048) {
            encode_chunk(aac, pending.data() + used, 1024, anchorPts + (int64_t)(encoded * 10000000 / rate), rate);
            encoded += 1024;
            used += 2048;
        }
        pending.erase(pending.begin(), pending.begin() + used);
    }
}

void audio_install() {
    if (g_cfg.wasapi) install_wasapi();
    if (g_cfg.asio) install_asio();
}

void audio_start() {
    std::thread(audio_worker).detach();
}
