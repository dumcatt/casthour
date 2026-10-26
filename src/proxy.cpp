// nvEncodeAPI64.dll proxy: the game imports this DLL statically, so it is loaded before the game
// creates its D3D device or audio stream. Every NVENC call goes to the real driver DLL.
//   mode=live      our own capture of every frame (menus, demo play, songs) + tapped game audio
//   mode=recorder  forward only what the game's recorder writes to its MP4 (mfhook.cpp)
#include "common.h"
#include "audio.h"
#include "capture.h"
#include "nvEncodeAPI.h"
#include "output.h"
#include <mutex>
#include <thread>
#include <unordered_map>

void install_mf_hook();

typedef NVENCSTATUS (NVENCAPI* PFN_CreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS (NVENCAPI* PFN_GetMaxVersion)(uint32_t*);

static PFN_CreateInstance       g_realCreateInstance;
static PFN_GetMaxVersion        g_realGetMaxVersion;
static PNVENCENCODEPICTURE      g_realEncodePicture;
static PNVENCREGISTERRESOURCE   g_realRegister;
static PNVENCUNREGISTERRESOURCE g_realUnregister;
static PNVENCMAPINPUTRESOURCE   g_realMap;
static PNVENCDESTROYENCODER     g_realDestroy;
static std::once_flag           g_initOnce;
static int64_t                  g_lastForcedIdr;

// Which D3D surface the game's recorder is encoding, to stream its clean frame in live mode.
static std::mutex g_resMtx;
static std::unordered_map<void*, IDirect3DSurface9*> g_regSurf;   // registered -> surface
static std::unordered_map<void*, void*>              g_mapReg;    // mapped -> registered

bool real_nvenc_create(NV_ENCODE_API_FUNCTION_LIST* fl) {
    return g_realCreateInstance && g_realCreateInstance(fl) == NV_ENC_SUCCESS;
}

static void init() {
    load_config();
    std::wstring path = g_cfg.realDll;
    if (path.empty()) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        path = std::wstring(sys) + L"\\nvEncodeAPI64.dll";
    }
    HMODULE real = LoadLibraryW(path.c_str());
    if (!real || real == g_self) {
        logf("cannot load real NVENC %ls (err %lu); encoding unavailable", path.c_str(), GetLastError());
    } else {
        g_realCreateInstance = (PFN_CreateInstance)GetProcAddress(real, "NvEncodeAPICreateInstance");
        g_realGetMaxVersion  = (PFN_GetMaxVersion)GetProcAddress(real, "NvEncodeAPIGetMaxSupportedVersion");
        logf("real NVENC: %ls", path.c_str());
    }
    if (g_cfg.live) {
        logf("mode: live (own capture of every frame)");
        // Own STA thread: ASIO drivers are apartment-threaded, and init may run on a game thread.
        // Audio first: the game opens its audio stream early, and creating the throwaway D3D
        // devices can take seconds under spice2x.
        std::thread([] {
            HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            int64_t t0 = qpc_100ns();
            if (g_cfg.audio) audio_install();
            int64_t t1 = qpc_100ns();
            capture_install();
            logf("hooks installed: audio %lld ms, video %lld ms", (t1 - t0) / 10000, (qpc_100ns() - t1) / 10000);
            if (SUCCEEDED(co)) CoUninitialize();
        }).join();
        if (g_cfg.audio) audio_start();
    } else {
        logf("mode: recorder (forwarding the game's own recording)");
        install_mf_hook();
    }
    output_start();
}

static bool want_idr() {
    if (g_cfg.live) return false;     // live mode keyframes come from our own encoder
    bool idr = output_take_idr_request();
    int64_t now = qpc_100ns();
    if (g_cfg.keyframeIntervalMs > 0 && now - g_lastForcedIdr >= (int64_t)g_cfg.keyframeIntervalMs * 10000)
        idr = true;
    if (idr) g_lastForcedIdr = now;
    return idr;
}

static NVENCSTATUS NVENCAPI Hook_RegisterResource(void* enc, NV_ENC_REGISTER_RESOURCE* r) {
    NVENCSTATUS st = g_realRegister(enc, r);
    if (st == NV_ENC_SUCCESS && r && r->resourceType == NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX) {
        std::lock_guard<std::mutex> lk(g_resMtx);
        g_regSurf[r->registeredResource] = (IDirect3DSurface9*)r->resourceToRegister;
    }
    return st;
}

static NVENCSTATUS NVENCAPI Hook_UnregisterResource(void* enc, NV_ENC_REGISTERED_PTR reg) {
    IDirect3DSurface9* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_resMtx);
        auto it = g_regSurf.find(reg);
        if (it != g_regSurf.end()) { s = it->second; g_regSurf.erase(it); }
    }
    capture_forget_game_surface(s);
    return g_realUnregister(enc, reg);
}

static NVENCSTATUS NVENCAPI Hook_MapInputResource(void* enc, NV_ENC_MAP_INPUT_RESOURCE* m) {
    NVENCSTATUS st = g_realMap(enc, m);
    if (st == NV_ENC_SUCCESS && m) {
        std::lock_guard<std::mutex> lk(g_resMtx);
        g_mapReg[m->mappedResource] = m->registeredResource;
    }
    return st;
}

static NVENCSTATUS NVENCAPI Hook_DestroyEncoder(void* enc) {
    NVENCSTATUS st = g_realDestroy(enc);
    capture_game_session_closed(enc);
    return st;
}

static NVENCSTATUS NVENCAPI Hook_EncodePicture(void* enc, NV_ENC_PIC_PARAMS* pp) {
    if (g_cfg.live && pp && pp->inputBuffer) {
        IDirect3DSurface9* s = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_resMtx);
            auto m = g_mapReg.find(pp->inputBuffer);
            if (m != g_mapReg.end()) {
                auto r = g_regSurf.find(m->second);
                if (r != g_regSurf.end()) s = r->second;
            }
        }
        capture_game_frame(s, enc);
    }
    if (!pp || (pp->encodePicFlags & NV_ENC_PIC_FLAG_EOS) || !want_idr())
        return g_realEncodePicture(enc, pp);
    // The game may reuse its params struct, so the forced flags are only applied to this call.
    uint32_t flags = pp->encodePicFlags;
    pp->encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    NVENCSTATUS st = g_realEncodePicture(enc, pp);
    pp->encodePicFlags = flags;
    return st;
}

NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t* version) {
    std::call_once(g_initOnce, init);
    if (!g_realGetMaxVersion) return NV_ENC_ERR_NO_ENCODE_DEVICE;
    return g_realGetMaxVersion(version);
}

NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST* fl) {
    std::call_once(g_initOnce, init);
    if (!g_realCreateInstance) return NV_ENC_ERR_NO_ENCODE_DEVICE;
    NVENCSTATUS st = g_realCreateInstance(fl);
    if (st != NV_ENC_SUCCESS || !fl) return st;
    if (fl->nvEncEncodePicture && fl->nvEncEncodePicture != Hook_EncodePicture) {
        g_realEncodePicture = fl->nvEncEncodePicture;
        fl->nvEncEncodePicture = Hook_EncodePicture;
    }
    if (g_cfg.live) {
        if (fl->nvEncRegisterResource && fl->nvEncRegisterResource != Hook_RegisterResource) {
            g_realRegister = fl->nvEncRegisterResource;
            fl->nvEncRegisterResource = Hook_RegisterResource;
        }
        if (fl->nvEncUnregisterResource && fl->nvEncUnregisterResource != Hook_UnregisterResource) {
            g_realUnregister = fl->nvEncUnregisterResource;
            fl->nvEncUnregisterResource = Hook_UnregisterResource;
        }
        if (fl->nvEncMapInputResource && fl->nvEncMapInputResource != Hook_MapInputResource) {
            g_realMap = fl->nvEncMapInputResource;
            fl->nvEncMapInputResource = Hook_MapInputResource;
        }
        if (fl->nvEncDestroyEncoder && fl->nvEncDestroyEncoder != Hook_DestroyEncoder) {
            g_realDestroy = fl->nvEncDestroyEncoder;
            fl->nvEncDestroyEncoder = Hook_DestroyEncoder;
        }
    }
    return st;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Hooks must be in place before the game creates its device and audio stream, which it
        // does well after loading us. The thread starts once the loader lock is released.
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            std::call_once(g_initOnce, init);
            return 0;
        }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
