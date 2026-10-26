#define NOMINMAX
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "capture.h"
#include "common.h"
#include "nvEncodeAPI.h"
#include "output.h"

bool real_nvenc_create(NV_ENCODE_API_FUNCTION_LIST* fl);   // proxy.cpp

enum { kReset = 16, kPresent = 17, kPresentEx = 121, kResetEx = 132, kSwapPresent = 3 };

typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_PresentEx)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResetEx)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SwapPresent)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

// spice2x hands the game a wrapper device; NVENC needs the real one behind it.
static const GUID IID_WrappedIDirect3DDevice9 =
    { 0x6dec0d40, 0x1339, 0x4bda, { 0xa5, 0xf2, 0x22, 0x31, 0xd4, 0x01, 0x0f, 0xd1 } };

static IDirect3DDevice9* unwrap_device(IDirect3DDevice9* dev) {
    void* probe = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_WrappedIDirect3DDevice9, &probe))) {
        ((IUnknown*)probe)->Release();
        return *(IDirect3DDevice9**)((char*)dev + 16);
    }
    return dev;
}

// ---- the game's clean recorder frame ----------------------------------------------------------

static std::mutex         g_gameMtx;
static IDirect3DSurface9* g_gameSurf;        // AddRef'd
static int64_t            g_gameTime;
static void*              g_gameEnc;         // the game encoder being followed
static UINT               g_gameArea;
static std::vector<void*> g_gameEncsSeen;

// The game runs two recorder encoders per song. Following whichever submitted last would mix
// their pictures, so stick to one: the largest, while it keeps submitting.
void capture_game_frame(IDirect3DSurface9* s, void* enc) {
    if (!s) return;
    D3DSURFACE_DESC d = {};
    s->GetDesc(&d);
    UINT area = d.Width * d.Height;
    int64_t now = qpc_100ns();
    std::lock_guard<std::mutex> lk(g_gameMtx);
    if (std::find(g_gameEncsSeen.begin(), g_gameEncsSeen.end(), enc) == g_gameEncsSeen.end()) {
        if (g_gameEncsSeen.size() > 64) g_gameEncsSeen.clear();
        g_gameEncsSeen.push_back(enc);
        logf("capture: game encoder %p encodes %ux%u surfaces (format %d)", enc, d.Width, d.Height, (int)d.Format);
    }
    if (enc != g_gameEnc) {
        bool current = g_gameEnc && now - g_gameTime < 3000000;
        if (current && area <= g_gameArea) return;
        g_gameEnc = enc;
        g_gameArea = area;
        logf("capture: following game encoder %p (%ux%u)", enc, d.Width, d.Height);
    }
    if (g_gameSurf != s) {
        if (g_gameSurf) g_gameSurf->Release();
        g_gameSurf = s;
        s->AddRef();
    }
    g_gameTime = now;
}

void capture_forget_game_surface(IDirect3DSurface9* s) {
    std::lock_guard<std::mutex> lk(g_gameMtx);
    if (s && g_gameSurf == s) { g_gameSurf->Release(); g_gameSurf = nullptr; }
}

static std::atomic<bool> g_refreshInputs{ false };

void capture_game_session_closed(void* enc) {
    g_refreshInputs = true;
    std::lock_guard<std::mutex> lk(g_gameMtx);
    if (enc == g_gameEnc) g_gameEnc = nullptr;
}

static void drop_game_surface() {
    std::lock_guard<std::mutex> lk(g_gameMtx);
    if (g_gameSurf) { g_gameSurf->Release(); g_gameSurf = nullptr; }
}

// ---- our encoder ------------------------------------------------------------------------------

static const int kSlots = 4;

struct Slot {
    IDirect3DSurface9*    rt = nullptr;
    NV_ENC_REGISTERED_PTR reg = nullptr;
    NV_ENC_INPUT_PTR      mapped = nullptr;
    NV_ENC_OUTPUT_PTR     bs = nullptr;
    HANDLE                ev = nullptr;
    int64_t               pts = 0;
    std::atomic<int>      state{ 0 };   // 0 free, 1 encoding, 2 encoded (render thread unmaps)
};

static NV_ENCODE_API_FUNCTION_LIST g_nv;
static void*              g_enc;
static IDirect3DDevice9*  g_encDev;
static int                g_encW, g_encH;
static bool               g_async;
static Slot               g_slots[kSlots];
static IDirect3DSurface9* g_scratch;
static std::mutex         g_nvMtx;
static std::mutex         g_qMtx;
static std::condition_variable g_qCv;
static std::deque<int>    g_q;
static std::atomic<int>   g_inflight{ 0 };
static int64_t            g_encRetryAt;

static void repeat_worker();

// spice2x runs the game at realtime priority; keep our threads ahead of its loading threads.
static void enc_worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        int i;
        {
            std::unique_lock<std::mutex> lk(g_qMtx);
            g_qCv.wait(lk, [] { return !g_q.empty(); });
            i = g_q.front();
            g_q.pop_front();
        }
        Slot& s = g_slots[i];
        if (g_async) WaitForSingleObject(s.ev, 2000);
        Item it;
        it.kind = ItemKind::Video;
        it.session = live_session();
        it.pts = it.dts = it.wall = s.pts;
        {
            std::lock_guard<std::mutex> lk(g_nvMtx);
            NV_ENC_LOCK_BITSTREAM lb = {};
            lb.version = NV_ENC_LOCK_BITSTREAM_VER;
            lb.outputBitstream = s.bs;
            if (g_nv.nvEncLockBitstream(g_enc, &lb) == NV_ENC_SUCCESS) {
                auto* p = (const uint8_t*)lb.bitstreamBufferPtr;
                it.data.assign(p, p + lb.bitstreamSizeInBytes);
                it.key = lb.pictureType == NV_ENC_PIC_TYPE_IDR;
                g_nv.nvEncUnlockBitstream(g_enc, s.bs);
            }
        }
        s.state = 2;
        if (!it.data.empty()) output_push(std::move(it));
        --g_inflight;
    }
}

static std::mutex           g_capMtx;            // encoder state: Present hook vs. repeat thread
static std::atomic<int>     g_lastSlot{ -1 };    // slot holding the most recent frame
static std::atomic<int64_t> g_lastSubmit{ 0 };     // any encode, including repeats
static std::atomic<int64_t> g_lastCapture{ 0 };    // last encode of a frame the game presented
static bool                 g_encDevMT;          // device created with D3DCREATE_MULTITHREADED

// Unmaps slots the worker finished encoding. Caller holds g_capMtx.
static void reclaim_slots() {
    for (auto& s : g_slots) {
        if (s.state != 2) continue;
        std::lock_guard<std::mutex> lk(g_nvMtx);
        if (s.mapped) g_nv.nvEncUnmapInputResource(g_enc, s.mapped);
        s.mapped = nullptr;
        s.state = 0;
    }
}

// Maps and encodes one slot's surface. Caller holds g_capMtx.
static bool submit_slot(Slot* slot, int64_t pts) {
    std::unique_lock<std::mutex> lk(g_nvMtx, std::try_to_lock);
    if (!lk) return false;
    NV_ENC_MAP_INPUT_RESOURCE mp = {};
    mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mp.registeredResource = slot->reg;
    if (g_nv.nvEncMapInputResource(g_enc, &mp) != NV_ENC_SUCCESS) return false;
    slot->mapped = mp.mappedResource;

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = g_encW; pic.inputHeight = g_encH;
    pic.inputBuffer = slot->mapped;
    pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    pic.outputBitstream = slot->bs;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = (uint64_t)pts;
    pic.completionEvent = g_async ? slot->ev : nullptr;
    if (output_take_idr_request()) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    if (g_nv.nvEncEncodePicture(g_enc, &pic) != NV_ENC_SUCCESS) {
        g_nv.nvEncUnmapInputResource(g_enc, slot->mapped);
        slot->mapped = nullptr;
        return false;
    }
    lk.unlock();
    slot->pts = pts;
    slot->state = 1;
    ++g_inflight;
    g_lastSlot = (int)(slot - g_slots);
    g_lastSubmit = pts;
    {
        std::lock_guard<std::mutex> qlk(g_qMtx);
        g_q.push_back((int)(slot - g_slots));
    }
    g_qCv.notify_one();
    return true;
}

static void enc_close() {
    if (!g_enc) return;
    g_lastSlot = -1;
    for (int t = 0; t < 2000 && g_inflight > 0; ++t) Sleep(1);
    std::lock_guard<std::mutex> lk(g_nvMtx);
    for (auto& s : g_slots) {
        if (s.mapped) g_nv.nvEncUnmapInputResource(g_enc, s.mapped);
        if (s.reg) g_nv.nvEncUnregisterResource(g_enc, s.reg);
        if (s.bs) g_nv.nvEncDestroyBitstreamBuffer(g_enc, s.bs);
        if (s.ev) {
            if (g_async) {
                NV_ENC_EVENT_PARAMS ep = {}; ep.version = NV_ENC_EVENT_PARAMS_VER; ep.completionEvent = s.ev;
                g_nv.nvEncUnregisterAsyncEvent(g_enc, &ep);
            }
            CloseHandle(s.ev);
        }
        if (s.rt) s.rt->Release();
        s.rt = nullptr; s.reg = nullptr; s.mapped = nullptr; s.bs = nullptr; s.ev = nullptr; s.state = 0;
    }
    if (g_scratch) { g_scratch->Release(); g_scratch = nullptr; }
    g_nv.nvEncDestroyEncoder(g_enc);
    g_enc = nullptr;
    g_encDev = nullptr;
    logf("encoder closed");
}

static bool enc_open(IDirect3DDevice9* dev, int w, int h) {
    if (!g_nv.nvEncOpenEncodeSessionEx) {
        g_nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (!real_nvenc_create(&g_nv)) { logf("encoder: real NVENC unavailable"); return false; }
    }
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {};
    op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.device = unwrap_device(dev);
    op.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS st = g_nv.nvEncOpenEncodeSessionEx(&op, &g_enc);
    if (st != NV_ENC_SUCCESS) { logf("encoder: OpenEncodeSessionEx failed %d", st); g_enc = nullptr; return false; }

    NV_ENC_CAPS_PARAM cp = {};
    cp.version = NV_ENC_CAPS_PARAM_VER;
    cp.capsToQuery = NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;
    int asyncCap = 0;
    g_nv.nvEncGetEncodeCaps(g_enc, NV_ENC_CODEC_H264_GUID, &cp, &asyncCap);
    g_async = asyncCap != 0;

    NV_ENC_PRESET_CONFIG pc = {};
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    g_nv.nvEncGetEncodePresetConfigEx(g_enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
                                      NV_ENC_TUNING_INFO_LOW_LATENCY, &pc);
    NV_ENC_CONFIG cfg = pc.presetCfg;
    uint32_t gop = (uint32_t)(g_cfg.fps * g_cfg.gopSeconds);
    cfg.gopLength = gop;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = (uint32_t)g_cfg.bitrateKbps * 1000;
    cfg.rcParams.maxBitRate = cfg.rcParams.averageBitRate;
    cfg.rcParams.vbvBufferSize = cfg.rcParams.averageBitRate / 2;
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
    cfg.encodeCodecConfig.h264Config.idrPeriod = gop;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    NV_ENC_INITIALIZE_PARAMS ip = {};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P4_GUID;
    ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    ip.encodeWidth = w; ip.encodeHeight = h; ip.darWidth = w; ip.darHeight = h;
    ip.frameRateNum = g_cfg.fps; ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.enableEncodeAsync = g_async ? 1 : 0;
    ip.encodeConfig = &cfg;
    st = g_nv.nvEncInitializeEncoder(g_enc, &ip);
    if (st != NV_ENC_SUCCESS && g_async) {
        g_async = false;
        ip.enableEncodeAsync = 0;
        st = g_nv.nvEncInitializeEncoder(g_enc, &ip);
    }
    if (st != NV_ENC_SUCCESS) { logf("encoder: InitializeEncoder failed %d", st); enc_close(); return false; }

    std::vector<uint8_t> seq(1024);
    uint32_t seqSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sp = {};
    sp.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sp.inBufferSize = (uint32_t)seq.size(); sp.spsppsBuffer = seq.data(); sp.outSPSPPSPayloadSize = &seqSize;
    if (g_nv.nvEncGetSequenceParams(g_enc, &sp) == NV_ENC_SUCCESS) seq.resize(seqSize); else seq.clear();

    for (auto& s : g_slots) {
        if (FAILED(dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, nullptr))) {
            logf("encoder: CreateRenderTarget failed"); enc_close(); return false;
        }
        NV_ENC_REGISTER_RESOURCE rr = {};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.resourceToRegister = s.rt;
        rr.width = w; rr.height = h;
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        if (g_nv.nvEncRegisterResource(g_enc, &rr) != NV_ENC_SUCCESS) { logf("encoder: RegisterResource failed"); enc_close(); return false; }
        s.reg = rr.registeredResource;
        NV_ENC_CREATE_BITSTREAM_BUFFER bb = {};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (g_nv.nvEncCreateBitstreamBuffer(g_enc, &bb) != NV_ENC_SUCCESS) { logf("encoder: CreateBitstreamBuffer failed"); enc_close(); return false; }
        s.bs = bb.bitstreamBuffer;
        if (g_async) {
            s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            NV_ENC_EVENT_PARAMS ep = {}; ep.version = NV_ENC_EVENT_PARAMS_VER; ep.completionEvent = s.ev;
            g_nv.nvEncRegisterAsyncEvent(g_enc, &ep);
        }
    }
    dev->CreateRenderTarget(w / 16 + 2, h / 16 + 2, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_scratch, nullptr);

    static std::once_flag workerOnce;
    std::call_once(workerOnce, [] {
        std::thread(enc_worker).detach();
        std::thread(repeat_worker).detach();
    });
    D3DDEVICE_CREATION_PARAMETERS created = {};
    dev->GetCreationParameters(&created);
    g_encDevMT = (created.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    g_encDev = dev; g_encW = w; g_encH = h;
    logf("encoder: %dx%d @ %d fps, %d kbps CBR, GOP %u, %s", w, h, g_cfg.fps, g_cfg.bitrateKbps, gop,
         g_async ? "async" : "sync");
    live_update_video(seq);
    return true;
}

// ---- masks ------------------------------------------------------------------------------------

static void apply_masks(IDirect3DDevice9* dev, IDirect3DSurface9* rt, int w, int h) {
    for (const Mask& m : g_cfg.masks) {
        RECT r = { m.x * w / 1920, m.y * h / 1080, (m.x + m.w) * w / 1920, (m.y + m.h) * h / 1080 };
        r.left = std::max<LONG>(0, r.left); r.top = std::max<LONG>(0, r.top);
        r.right = std::min<LONG>(w, r.right); r.bottom = std::min<LONG>(h, r.bottom);
        if (r.right <= r.left || r.bottom <= r.top) continue;
        if (g_cfg.maskPreview) {
            const LONG t = 3;
            RECT e[4] = { { r.left, r.top, r.right, std::min(r.bottom, r.top + t) },
                          { r.left, std::max(r.top, r.bottom - t), r.right, r.bottom },
                          { r.left, r.top, std::min(r.right, r.left + t), r.bottom },
                          { std::max(r.left, r.right - t), r.top, r.right, r.bottom } };
            for (auto& q : e) dev->ColorFill(rt, &q, D3DCOLOR_XRGB(0, 255, 0));
        } else if (m.pixelate && g_scratch) {
            RECT tiny = { 0, 0, std::max<LONG>(1, (r.right - r.left) / 16), std::max<LONG>(1, (r.bottom - r.top) / 16) };
            dev->StretchRect(rt, &r, g_scratch, &tiny, D3DTEXF_POINT);
            dev->StretchRect(g_scratch, &tiny, rt, &r, D3DTEXF_POINT);
        } else {
            dev->ColorFill(rt, &r, D3DCOLOR_XRGB(0, 0, 0));
        }
    }
}

// ---- frame capture (game's render thread, inside Present) -------------------------------------

static IDirect3DDevice9* g_capDev;
static int64_t           g_capSeen;
static int64_t           g_nextDue;
static int               g_lastSource = -1;    // 0 screen, 1 game recorder frame
static IDirect3DSurface9* g_gameSurfChecked;
static bool              g_gameSurfUsable;

static void capture(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    int64_t now = qpc_100ns();
    if (g_capDev && g_capDev != dev) {
        if (now - g_capSeen < 20000000) return;     // stick to the device that presents
        logf("capture: switching device");
        enc_close();
    }
    if (g_capDev != dev) logf("capture: following device %p", (void*)dev);
    g_capDev = dev;
    g_capSeen = now;
    if (!output_active()) return;

    // Pace to the target fps on the game's (faster) present cadence, without catch-up bursts.
    static int64_t lastCap;
    int64_t period = 10000000 / g_cfg.fps;
    if (now < g_nextDue - 20000 || now - lastCap < period * 3 / 4) return;
    g_nextDue = (now - g_nextDue > period) ? now + period : g_nextDue + period;
    lastCap = now;

    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = sc ? sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                    : dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    int w = (g_cfg.width > 0 ? g_cfg.width : (int)d.Width) & ~1;
    int h = (g_cfg.height > 0 ? g_cfg.height : (int)d.Height) & ~1;

    // The repeat thread may be submitting during a stall; never make the game wait for it.
    std::unique_lock<std::mutex> capLk(g_capMtx, std::try_to_lock);
    if (!capLk) { bb->Release(); return; }
    if (g_enc && (g_encDev != dev || g_encW != w || g_encH != h)) enc_close();
    if (!g_enc && now >= g_encRetryAt && !enc_open(dev, w, h)) g_encRetryAt = now + 50000000;
    if (!g_enc) { bb->Release(); return; }

    reclaim_slots();
    if (g_refreshInputs.exchange(false)) {
        std::lock_guard<std::mutex> lk(g_nvMtx);
        int n = 0;
        for (auto& s : g_slots) {
            if (s.state != 0 || !s.reg) continue;
            g_nv.nvEncUnregisterResource(g_enc, s.reg);
            s.reg = nullptr;
            NV_ENC_REGISTER_RESOURCE rr = {};
            rr.version = NV_ENC_REGISTER_RESOURCE_VER;
            rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            rr.resourceToRegister = s.rt;
            rr.width = w; rr.height = h;
            rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
            if (g_nv.nvEncRegisterResource(g_enc, &rr) == NV_ENC_SUCCESS) { s.reg = rr.registeredResource; ++n; }
            else s.state = 3;   // unusable until the encoder is reopened
        }
        logf("capture: game closed an NVENC session, re-registered %d input(s)", n);
    }
    Slot* slot = nullptr;
    for (auto& s : g_slots) if (s.state == 0) { slot = &s; break; }
    if (!slot) { bb->Release(); return; }        // encoder behind: drop this frame

    bool fromGame = false;
    if (g_cfg.useGameFrame) {
        std::lock_guard<std::mutex> lk(g_gameMtx);
        if (g_gameSurf && now - g_gameTime < 1500000) {
            if (g_gameSurfChecked != g_gameSurf) {
                IDirect3DDevice9* owner = nullptr;
                g_gameSurf->GetDevice(&owner);
                g_gameSurfUsable = owner == dev;
                if (owner) owner->Release();
                g_gameSurfChecked = g_gameSurf;
                if (!g_gameSurfUsable) logf("capture: recorder surface belongs to another device, not used");
            }
            if (g_gameSurfUsable)
                fromGame = SUCCEEDED(dev->StretchRect(g_gameSurf, nullptr, slot->rt, nullptr, D3DTEXF_LINEAR));
        }
    }
    if (!fromGame) {
        HRESULT shr = dev->StretchRect(bb, nullptr, slot->rt, nullptr, D3DTEXF_LINEAR);
        static int fails = 0;
        if (FAILED(shr) && fails++ < 5) logf("capture: StretchRect(backbuffer) failed 0x%08lX", (unsigned long)shr);
    }
    bb->Release();
    if (!fromGame || g_cfg.maskGameFrame || g_cfg.maskPreview) apply_masks(dev, slot->rt, w, h);
    if (g_lastSource != (int)fromGame) {
        logf("capture: source is now %s", fromGame ? "the game's recorder frame" : "the screen (masks applied)");
        g_lastSource = (int)fromGame;
    }

    if (submit_slot(slot, now)) g_lastCapture = now;
}

// A fault in our capture must never take the game down.
static void capture_safe(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    static bool faulted = false;
    if (faulted) return;
    __try { capture(dev, sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; logf("capture: fault inside Present hook, capture disabled"); }
}

static void release_for_reset(IDirect3DDevice9* dev) {
    std::lock_guard<std::mutex> lk(g_capMtx);
    if (dev == g_encDev) enc_close();
    drop_game_surface();
    g_gameSurfChecked = nullptr;
}

// While the game isn't presenting (loading screens), re-encode the last frame at the target fps so
// viewers get a steady stream and their audio doesn't stall waiting for video. Only on devices
// created thread-safe (the game's is), since NVENC touches the D3D9 input from this thread.
static void repeat_worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // Sleep() granularity is ~16 ms by default; a high-resolution timer keeps repeats at full fps.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    uint64_t repeats = 0;
    int64_t nextLog = qpc_100ns() + 100000000;
    for (;;) {
        int64_t period = 10000000 / g_cfg.fps;
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -std::max<int64_t>(10000, period / 4);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            Sleep(1);
        }
        int64_t now = qpc_100ns();
        if (now >= nextLog) {
            if (repeats) logf("video: repeated the last frame %llu times while the game wasn't presenting", (unsigned long long)repeats);
            repeats = 0;
            nextLog = now + 100000000;
        }
        // A stall is two frame periods without a real capture; then fill at the normal frame rate.
        if (!g_enc || !g_encDevMT || !output_active() || now - g_lastCapture.load() < period * 2 ||
            now - g_lastSubmit.load() < period - 10000) continue;
        std::unique_lock<std::mutex> lk(g_capMtx, std::try_to_lock);
        if (!lk || !g_enc) continue;
        int i = g_lastSlot.load();
        if (i < 0) continue;
        reclaim_slots();
        if (g_slots[i].state == 0 && submit_slot(&g_slots[i], now)) ++repeats;
    }
}

// ---- hooks ------------------------------------------------------------------------------------
//
// Other overlays also hook Present. Nahimic's OSD (shipped with ASUS Xonar drivers, injected into
// every process) calls through the vtable whenever the slot no longer points into d3d9.dll, which
// loops forever with a vtable hook. So vtables are left alone and d3d9's code is hooked instead
// (see code_hook): an entry another hook already redirected is chained, a plain prologue gets a
// trampoline. A vtable hook is only the fallback for an entry that can't be patched safely.

static thread_local int t_inPresent;
static void* g_d3d9Fn[kResetEx + 1];         // what to call next per slot index
static bool  g_thunked[kResetEx + 1];        // slot's implementation is hooked in code

static bool in_d3d9(const void* p) {
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)p, &m) && m == GetModuleHandleW(L"d3d9.dll");
}

static void* next_fn(void* obj, int slot) {
    if (g_thunked[slot]) return g_d3d9Fn[slot];
    void* o = t_inPresent > 1 ? nullptr : vt_orig(obj, slot);
    if (!o) {
        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true))
            logf("Present reached %s; calling d3d9 directly to avoid a hook loop",
                 t_inPresent > 1 ? "again on the same thread (another overlay chains back to us)" : "through a vtable copy");
        o = g_d3d9Fn[slot];
    }
    return o;
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (!t_inPresent++) capture_safe(d, nullptr);
    HRESULT hr = ((PFN_Present)next_fn(d, kPresent))(d, a, b, w, r);
    --t_inPresent;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_PresentEx(IDirect3DDevice9Ex* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
    if (!t_inPresent++) capture_safe(d, nullptr);
    HRESULT hr = ((PFN_PresentEx)next_fn(d, kPresentEx))(d, a, b, w, r, f);
    --t_inPresent;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_SwapPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
    if (!t_inPresent++) {
        IDirect3DDevice9* d = nullptr;
        if (SUCCEEDED(sc->GetDevice(&d)) && d) {
            IDirect3DSwapChain9* sc0 = nullptr;
            if (SUCCEEDED(d->GetSwapChain(0, &sc0)) && sc0) {
                if (sc0 == sc) capture_safe(d, sc);     // head 0 only (TDJ also has a subscreen head)
                sc0->Release();
            }
            d->Release();
        }
    }
    HRESULT hr = ((PFN_SwapPresent)next_fn(sc, kSwapPresent))(sc, a, b, w, r, f);
    --t_inPresent;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    release_for_reset(d);
    return ((PFN_Reset)next_fn(d, kReset))(d, pp);
}

static HRESULT STDMETHODCALLTYPE Hook_ResetEx(IDirect3DDevice9Ex* d, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m) {
    release_for_reset(d);
    return ((PFN_ResetEx)next_fn(d, kResetEx))(d, pp, m);
}

// Hooks d3d9's implementation behind a vtable slot in code, leaving the vtable untouched; the
// vtable hook is a last resort for an entry we can't safely patch.
static void hook_slot(void* obj, int slot, void* repl, const char* what) {
    void* fn = (*(void***)obj)[slot];
    if (g_thunked[slot] || fn == repl) return;
    if (in_d3d9(fn)) {
        bool chained = *(uint8_t*)fn == 0xE9;
        if (void* next = code_hook(fn, repl)) {
            g_d3d9Fn[slot] = next;
            g_thunked[slot] = true;
            logf("  %s: hooked in code%s", what, chained ? " (chained behind another hook)" : "");
            return;
        }
        if (!g_d3d9Fn[slot]) g_d3d9Fn[slot] = fn;
    }
    vt_hook(obj, slot, repl);
    logf("  %s: vtable hook (may conflict with overlays such as Nahimic)", what);
}

static void hook_device(IDirect3DDevice9* dev) {
    hook_slot(dev, kReset, (void*)&Hook_Reset, "Reset");
    hook_slot(dev, kPresent, (void*)&Hook_Present, "Present");
    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex)) && ex) {
        hook_slot(ex, kPresentEx, (void*)&Hook_PresentEx, "PresentEx");
        hook_slot(ex, kResetEx, (void*)&Hook_ResetEx, "ResetEx");
        ex->Release();
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        hook_slot(sc, kSwapPresent, (void*)&Hook_SwapPresent, "SwapChain::Present");
        sc->Release();
    }
}

void capture_install() {
    HWND wnd = CreateWindowExW(0, L"STATIC", L"nvenc-proxy", WS_POPUP, 0, 0, 64, 64,
                               nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.hDeviceWindow = wnd;
    int hooked = 0;

    IDirect3D9Ex* d3dex = nullptr;
    if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dex)) && d3dex) {
        IDirect3DDevice9Ex* dev = nullptr;
        if (SUCCEEDED(d3dex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, nullptr, &dev)) && dev) {
            hook_device(dev);
            dev->Release();
            ++hooked;
        }
        d3dex->Release();
    }
    if (IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION)) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &dev)) && dev) {
            hook_device(dev);
            dev->Release();
            ++hooked;
        }
        d3d->Release();
    }
    if (wnd) DestroyWindow(wnd);
    logf("D3D9 Present hooked via %d dummy device(s); %zu mask(s) configured%s", hooked, g_cfg.masks.size(),
         g_cfg.maskPreview ? " (preview: outlines only)" : "");
}
