#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct Mask {
    std::string name;
    int  x = 0, y = 0, w = 0, h = 0;      // 1920x1080 reference pixels, scaled to the output size
    bool pixelate = false;                // false = solid black
};

struct Config {
    // [proxy]
    std::wstring realDll;                 // empty -> %SystemRoot%\System32\nvEncodeAPI64.dll
    bool         log = true;
    bool         live = true;             // mode=live: own capture of every frame; mode=recorder: tap the game's MP4
    // [output]
    std::string  tcpListen;               // "host:port" OBS connects to; empty disables
    std::string  udpTarget;               // "host:port" to push MPEG-TS to; empty disables
    std::wstring file;                    // local .ts path ({time} is expanded); empty disables
    int          keyframeIntervalMs = 0;  // recorder mode only
    // [capture]
    int  fps = 60, width = 0, height = 0, bitrateKbps = 12000, gopSeconds = 2;
    bool useGameFrame = true;             // stream the recorder's clean frame while the game records
    bool maskGameFrame = false;           // also apply masks to that clean frame
    bool maskPreview = false;             // outline masks in red instead of hiding
    std::vector<Mask> masks;
    // [audio]
    bool audio = true, asio = true, wasapi = true, asioProbe = true;
    int  audioBitrateKbps = 192;
    int  audioOffsetMs = 0;
};

extern Config  g_cfg;
extern HMODULE g_self;

void    load_config();
void    logf(const char* fmt, ...);
int64_t qpc_100ns();

// Patches `slot` in obj's vtable (once per vtable); vt_orig returns the original for obj's vtable.
void* vt_hook(void* obj, int slot, void* repl);
void* vt_orig(void* obj, int slot);
// Redirects every IAT entry equal to `target` in all loaded modules except this one.
int   iat_hook_all(void* target, void* repl, const char* what);
// Hooks code at `fn` so it jumps to `repl`; returns what to call to continue, or null if the
// entry can't be patched safely. An entry that is already `jmp rel32` is retargeted in place.
void* code_hook(void* fn, void* repl);
// Base name of the module containing `p` ("?" if none).
std::string module_name(const void* p);
