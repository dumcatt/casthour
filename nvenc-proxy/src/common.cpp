#include "common.h"
#include <psapi.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

Config  g_cfg;
HMODULE g_self = nullptr;

static std::mutex g_logMtx;
static FILE*      g_log = nullptr;

static std::wstring self_dir() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

static std::wstring ini_str(const std::wstring& ini, const wchar_t* sec, const wchar_t* key,
                            const wchar_t* def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(sec, key, def, buf, 1024, ini.c_str());
    return buf;
}

static int ini_int(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, ini.c_str());
}

static std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (char)c;
    return s;
}

static std::wstring resolve(const std::wstring& dir, const std::wstring& p) {
    if (p.empty() || p.find(L':') != std::wstring::npos || p[0] == L'\\' || p[0] == L'/') return p;
    return dir + L"\\" + p;
}

// [masks] lines look like  credits=760,1030,400,50,pixelate
static void load_masks(const std::wstring& ini) {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetPrivateProfileSectionW(L"masks", buf.data(), (DWORD)buf.size(), ini.c_str());
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        std::wstring line(p);
        if (line[0] == L';' || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        Mask m;
        m.name = narrow(line.substr(0, eq));
        std::wstring v = line.substr(eq + 1);
        if (swscanf_s(v.c_str(), L"%d , %d , %d , %d", &m.x, &m.y, &m.w, &m.h) != 4 || m.w <= 0 || m.h <= 0) continue;
        m.pixelate = v.find(L"pixel") != std::wstring::npos;
        g_cfg.masks.push_back(m);
    }
}

void load_config() {
    std::wstring dir = self_dir();
    std::wstring ini = dir + L"\\nvenc-proxy.ini";
    g_cfg.realDll   = resolve(dir, ini_str(ini, L"proxy", L"real_dll", L""));
    g_cfg.log       = ini_int(ini, L"proxy", L"log", 1) != 0;
    g_cfg.live      = _wcsicmp(ini_str(ini, L"proxy", L"mode", L"live").c_str(), L"recorder") != 0;

    g_cfg.tcpListen = narrow(ini_str(ini, L"output", L"tcp_listen", L"0.0.0.0:9910"));
    g_cfg.udpTarget = narrow(ini_str(ini, L"output", L"udp_target", L""));
    g_cfg.file      = resolve(dir, ini_str(ini, L"output", L"file", L""));
    g_cfg.keyframeIntervalMs = ini_int(ini, L"output", L"keyframe_interval_ms", g_cfg.udpTarget.empty() ? 0 : 2000);

    g_cfg.fps           = ini_int(ini, L"capture", L"fps", 60);
    g_cfg.width         = ini_int(ini, L"capture", L"width", 0);
    g_cfg.height        = ini_int(ini, L"capture", L"height", 0);
    g_cfg.bitrateKbps   = ini_int(ini, L"capture", L"bitrate_kbps", 12000);
    g_cfg.gopSeconds    = ini_int(ini, L"capture", L"gop_seconds", 2);
    g_cfg.useGameFrame  = ini_int(ini, L"capture", L"use_game_frame", 1) != 0;
    g_cfg.maskGameFrame = ini_int(ini, L"capture", L"mask_game_frame", 0) != 0;
    g_cfg.maskPreview   = ini_int(ini, L"capture", L"mask_preview", 0) != 0;
    if (g_cfg.fps < 1) g_cfg.fps = 60;
    if (g_cfg.gopSeconds < 1) g_cfg.gopSeconds = 2;

    g_cfg.audio            = ini_int(ini, L"audio", L"enabled", 1) != 0;
    g_cfg.asio             = ini_int(ini, L"audio", L"asio", 1) != 0;
    g_cfg.wasapi           = ini_int(ini, L"audio", L"wasapi", 1) != 0;
    g_cfg.asioProbe        = ini_int(ini, L"audio", L"asio_probe", 1) != 0;
    g_cfg.audioBitrateKbps = ini_int(ini, L"audio", L"bitrate_kbps", 192);
    g_cfg.audioOffsetMs    = ini_int(ini, L"audio", L"offset_ms", 0);

    load_masks(ini);
    if (g_cfg.log) g_log = _wfsopen((dir + L"\\nvenc-proxy.log").c_str(), L"w", _SH_DENYNO);
}

void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::lock_guard<std::mutex> lk(g_logMtx);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

int64_t qpc_100ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / freq.QuadPart * 10000000 + c.QuadPart % freq.QuadPart * 10000000 / freq.QuadPart);
}

// ---- vtable hooks -----------------------------------------------------------------------------

struct VtEntry { void** vtbl; int slot; void* orig; };
static VtEntry          g_vt[256];
static std::atomic<int> g_vtCount{ 0 };
static std::mutex       g_vtMtx;

void* vt_orig(void* obj, int slot) {
    void** vtbl = *(void***)obj;
    int n = g_vtCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_vt[i].vtbl == vtbl && g_vt[i].slot == slot) return g_vt[i].orig;
    return nullptr;
}

void* vt_hook(void* obj, int slot, void* repl) {
    void** vtbl = *(void***)obj;
    std::lock_guard<std::mutex> lk(g_vtMtx);
    int n = g_vtCount.load();
    for (int i = 0; i < n; ++i) {
        if (g_vt[i].vtbl != vtbl || g_vt[i].slot != slot) continue;
        if (vtbl[slot] == repl) return g_vt[i].orig;
        // Same address but our patch is gone (module unloaded and reloaded): patch again.
        g_vt[i].orig = vtbl[slot];
        DWORD old;
        VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
        vtbl[slot] = repl;
        VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
        return g_vt[i].orig;
    }
    if (n >= 256 || vtbl[slot] == repl) return nullptr;
    // Publish the original before redirecting, so a hook firing immediately can find it.
    g_vt[n] = { vtbl, slot, vtbl[slot] };
    g_vtCount.store(n + 1, std::memory_order_release);
    DWORD old;
    VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
    vtbl[slot] = repl;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return g_vt[n].orig;
}

// ---- code hooks -------------------------------------------------------------------------------

// Executable pages for jump stubs and trampolines, each within rel32 reach of what it patches.
struct StubPage { uint8_t* base; int used; };
static std::vector<StubPage> g_pages;
static std::mutex            g_pageMtx;

static uint8_t* alloc_near(uint8_t* p, int size) {
    size = (size + 15) & ~15;
    std::lock_guard<std::mutex> lk(g_pageMtx);
    for (auto& pg : g_pages) {
        int64_t d = pg.base - p;
        if (d > -0x60000000LL && d < 0x60000000LL && pg.used + size <= 4096) {
            uint8_t* s = pg.base + pg.used;
            pg.used += size;
            return s;
        }
    }
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity, base = (uintptr_t)p & ~(gran - 1);
    uint8_t* page = nullptr;
    for (uintptr_t d = gran; d < 0x50000000 && !page; d += gran) {
        if (base > d) page = (uint8_t*)VirtualAlloc((void*)(base - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!page) page = (uint8_t*)VirtualAlloc((void*)(base + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!page) return nullptr;
    g_pages.push_back({ page, size });
    return page;
}

static void put_abs_jmp(uint8_t* s, const void* target) {   // jmp [rip+0]; dq target
    s[0] = 0xFF; s[1] = 0x25; *(int32_t*)(s + 2) = 0;
    *(const void**)(s + 6) = target;
}

// Length of common position-independent MSVC x64 prologue instructions; 0 for anything else.
static int prologue_insn_len(const uint8_t* p) {
    uint8_t b = p[0];
    if (b >= 0x50 && b <= 0x57) return 1;                                         // push r
    if ((b == 0x40 || b == 0x41) && p[1] >= 0x50 && p[1] <= 0x57) return 2;       // push r (REX)
    if (b == 0x48 || b == 0x49 || b == 0x4C || b == 0x4D) {
        uint8_t op = p[1], m = p[2], mod = m >> 6, rm = m & 7;
        if (op == 0x89 || op == 0x8B) {                                           // mov r/m, r
            if (mod == 3) return 3;
            if (rm == 4) {                                                        // SIB
                if ((p[3] & 7) == 5 && mod == 0) return 0;
                return mod == 1 ? 5 : mod == 2 ? 8 : 4;
            }
            if (rm == 5 && mod == 0) return 0;                                    // rip-relative
            return mod == 1 ? 4 : mod == 2 ? 7 : 3;
        }
        if (op == 0x83 && (m & 0xF8) == 0xE8) return 4;                           // sub r, imm8
        if (op == 0x81 && (m & 0xF8) == 0xE8) return 7;                           // sub r, imm32
    }
    return 0;
}

void* code_hook(void* fn, void* repl) {
    uint8_t* p = (uint8_t*)fn;
    uint8_t* entry = alloc_near(p, 16);
    if (!entry) return nullptr;
    put_abs_jmp(entry, repl);
    int64_t rel = entry - (p + 5);
    if (rel != (int32_t)rel) return nullptr;

    void* next;
    int patch = 5;
    if (p[0] == 0xE9) {
        next = p + 5 + *(int32_t*)(p + 1);
    } else {
        int n = 0;
        while (n < 5) {
            int len = prologue_insn_len(p + n);
            if (!len) return nullptr;
            n += len;
        }
        uint8_t* tramp = alloc_near(p, n + 14);
        if (!tramp) return nullptr;
        memcpy(tramp, p, n);
        put_abs_jmp(tramp + n, p + n);
        FlushInstructionCache(GetCurrentProcess(), tramp, n + 14);
        next = tramp;
        patch = n;
    }
    FlushInstructionCache(GetCurrentProcess(), entry, 16);
    DWORD old;
    VirtualProtect(p, patch, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)rel;
    for (int i = 5; i < patch; ++i) p[i] = 0x90;
    VirtualProtect(p, patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, patch);
    return next;
}

std::string module_name(const void* p) {
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)p, &m) && m)
        GetModuleFileNameW(m, name, MAX_PATH);
    std::wstring w(name);
    size_t k = w.find_last_of(L"\\/");
    if (k != std::wstring::npos) w = w.substr(k + 1);
    return narrow(w);
}

// ---- IAT hooks --------------------------------------------------------------------------------

static int patch_iat(HMODULE mod, void* target, void* repl) {
    auto base = (uint8_t*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int count = 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        for (auto th = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); th->u1.Function; ++th) {
            if ((void*)th->u1.Function != target) continue;
            DWORD old;
            VirtualProtect(&th->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            th->u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&th->u1.Function, sizeof(void*), old, &old);
            ++count;
        }
    }
    return count;
}

int iat_hook_all(void* target, void* repl, const char* what) {
    HMODULE mods[1024]; DWORD needed = 0;
    EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed);
    int patched = 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
        if (mods[i] == g_self) continue;
        int n = patch_iat(mods[i], target, repl);
        if (n) {
            wchar_t name[MAX_PATH] = L"?";
            GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH);
            logf("hooked %s in %ls", what, name);
            patched += n;
        }
    }
    return patched;
}
