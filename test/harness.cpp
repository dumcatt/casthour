// Behaves like the game for testing the proxy:
//  - presents 1280x720 frames at 120 Hz through swapchain 0, with a red "credits" box drawn on screen
//  - in the middle, runs two recorder encoders like the game's: NVENC (through the proxy) encodes
//    a clean surface without the box, plus a smaller solid-green one, into MP4s via Media
//    Foundation's MPEG-4 sink. The stream must never show green.
//  - then stops presenting for 1.5 s, like a loading screen
//  - plays a quiet tone through WASAPI (session muted, opened before or after the proxy loads) or
//    the first registered ASIO driver (about -60 dBFS)
//  - for A/V sync checks, flashes the top-left corner white once a second while (in asio mode) a
//    short tick is scheduled to play at the same instant
// Usage: harness.exe <proxy dll> <out prefix> <none|wasapi|wasapi-early|asio> [menu s=3] [record s=3] [delay ms=1500]
// HARNESS_ASIO_RATE=44100 asks the ASIO driver for that sample rate.
#include <windows.h>
#include <d3d9.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "nvEncodeAPI.h"

#define CHECK(x) do { HRESULT _hr = (x); if (FAILED(_hr)) { printf("%s failed 0x%08lX (line %d)\n", #x, (unsigned long)_hr, __LINE__); exit(1); } } while (0)
#define NVCHECK(x) do { NVENCSTATUS _s = (x); if (_s) { printf("%s failed %d (line %d)\n", #x, _s, __LINE__); exit(1); } } while (0)

static const int W = 1280, H = 720, HZ = 120, REC_FPS = 60, RATE = 48000;
static const RECT kCredits = { 540, 640, 740, 700 };   // = 810,960,300,90 in 1920x1080 reference
static NV_ENCODE_API_FUNCTION_LIST fl;

static void draw_scene(IDirect3DDevice9* dev, IDirect3DSurface9* s, int f, bool credits) {
    dev->ColorFill(s, nullptr, D3DCOLOR_ARGB(255, 20, 90, 160));
    int x = (f * 6) % (W - 160);
    RECT r = { x, 200, x + 160, 360 };
    dev->ColorFill(s, &r, D3DCOLOR_ARGB(255, 240, 240, 240));
    if (credits) {   // red box with small white "glyphs", so pixelation is visible
        dev->ColorFill(s, &kCredits, D3DCOLOR_ARGB(255, 255, 0, 0));
        for (LONG gx = kCredits.left + 6; gx + 4 <= kCredits.right; gx += 9)
            for (LONG gy = kCredits.top + 8; gy + 4 <= kCredits.bottom; gy += 13) {
                RECT g = { gx, gy, gx + 4, gy + 6 };
                dev->ColorFill(s, &g, D3DCOLOR_ARGB(255, 255, 255, 255));
            }
    }
}

// ---- audio output like the game ---------------------------------------------------------------

static double g_phase;
static inline float tone() { g_phase += 2 * 3.14159265 * 440.0 / RATE; return (float)(0.001 * sin(g_phase)); }

struct Wasapi {
    IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr; WAVEFORMATEX* fmt = nullptr; UINT32 size = 0;
    bool start() {
        IMMDeviceEnumerator* en = nullptr; IMMDevice* d = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return false;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return false;
        if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) return false;
        ac->GetMixFormat(&fmt);
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, fmt, nullptr))) return false;
        ac->GetBufferSize(&size);
        ac->GetService(IID_PPV_ARGS(&rc));
        ISimpleAudioVolume* vol = nullptr;   // muted at the session: the data we write is still tapped
        if (SUCCEEDED(ac->GetService(IID_PPV_ARGS(&vol)))) { vol->SetMasterVolume(0.0f, nullptr); vol->Release(); }
        fill();
        ac->Start();
        printf("WASAPI: %u Hz, %u ch, %u-bit\n", fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample);
        return true;
    }
    void fill() {   // float32 mix format assumed (shared mode default)
        UINT32 pad = 0; ac->GetCurrentPadding(&pad);
        UINT32 n = size - pad; if (!n) return;
        BYTE* p = nullptr; if (FAILED(rc->GetBuffer(n, &p))) return;
        float* f = (float*)p;
        for (UINT32 i = 0; i < n; ++i) { float v = tone(); for (int c = 0; c < fmt->nChannels; ++c) f[i * fmt->nChannels + c] = v; }
        rc->ReleaseBuffer(n, 0);
    }
};

typedef long ASIOBool;
struct ASIOBufferInfo { ASIOBool isInput; long channelNum; void* buffers[2]; };
struct ASIOChannelInfo { long channel; ASIOBool isInput; ASIOBool isActive; long channelGroup; long type; char name[32]; };
struct ASIOCallbacks {
    void  (*bufferSwitch)(long, ASIOBool);
    void  (*sampleRateDidChange)(double);
    long  (*asioMessage)(long, long, void*, double*);
    void* (*bufferSwitchTimeInfo)(void*, long, ASIOBool);
};
struct IASIO : public IUnknown {
    virtual ASIOBool init(void*) = 0;
    virtual void getDriverName(char*) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char*) = 0;
    virtual long start() = 0;
    virtual long stop() = 0;
    virtual long getChannels(long*, long*) = 0;
    virtual long getLatencies(long*, long*) = 0;
    virtual long getBufferSize(long*, long*, long*, long*) = 0;
    virtual long canSampleRate(double) = 0;
    virtual long getSampleRate(double*) = 0;
    virtual long setSampleRate(double) = 0;
    virtual long getClockSources(void*, long*) = 0;
    virtual long setClockSource(long) = 0;
    virtual long getSamplePosition(void*, void*) = 0;
    virtual long getChannelInfo(ASIOChannelInfo*) = 0;
    virtual long createBuffers(ASIOBufferInfo*, long, long, ASIOCallbacks*) = 0;
    virtual long disposeBuffers() = 0;
    virtual long controlPanel() = 0;
    virtual long future(long, void*) = 0;
    virtual long outputReady() = 0;
};

static ASIOBufferInfo g_asioBufs[2];
static long g_asioSize, g_asioType, g_asioOutLat;
static double g_asioRate;
// Sync ticks: a 10 ms burst whose start is scheduled to *play* on each whole second of the
// harness timeline, when the video shows its white flash.
static volatile int64_t g_flashT0, g_flashFreq;
static int64_t g_tickSample = -1, g_lastTickSec = -1;

static void asio_switch(long idx, ASIOBool) {
    LARGE_INTEGER nowq; QueryPerformanceCounter(&nowq);
    double playAt = g_flashFreq ? (double)(nowq.QuadPart - g_flashT0) / g_flashFreq + g_asioOutLat / g_asioRate : -1;
    for (long i = 0; i < g_asioSize; ++i) {
        float v = tone();
        if (g_flashFreq && playAt >= 0) {
            double t = playAt + i / g_asioRate;
            int64_t sec = (int64_t)t;
            if (sec != g_lastTickSec && t - sec < 0.004) { g_lastTickSec = sec; g_tickSample = 0; }
        }
        if (g_tickSample >= 0) {
            v += (float)(0.01 * sin(2 * 3.14159265 * 2000 * g_tickSample / g_asioRate));
            if (++g_tickSample > (int64_t)(g_asioRate / 100)) g_tickSample = -1;
        }
        for (int c = 0; c < 2; ++c) {
            void* b = g_asioBufs[c].buffers[idx];
            switch (g_asioType) {
            case 16: ((int16_t*)b)[i] = (int16_t)(v * 32767); break;
            case 18: ((int32_t*)b)[i] = (int32_t)(v * 2147483647.0); break;
            case 19: ((float*)b)[i] = v; break;
            case 17: { int32_t s = (int32_t)(v * 8388607); uint8_t* p = (uint8_t*)b + i * 3; p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); p[2] = (uint8_t)(s >> 16); break; }
            default: break;
            }
        }
    }
}
static void asio_rate(double) {}
static long asio_msg(long sel, long val, void*, double*) {
    if (sel == 1) return val == 2 ? 1 : 0;   // kAsioSelectorSupported: only kAsioEngineVersion
    if (sel == 2) return 2;                   // kAsioEngineVersion
    return 0;
}
static ASIOCallbacks g_asioCb = { asio_switch, asio_rate, asio_msg, nullptr };

static IASIO* start_asio(HWND wnd) {
    HKEY root; wchar_t name[256]; wchar_t clsid[64] = {}; DWORD sz = sizeof clsid; CLSID c;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) || RegEnumKeyW(root, 0, name, 256) ||
        RegGetValueW(root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &sz) || FAILED(CLSIDFromString(clsid, &c))) {
        printf("ASIO: no driver registered\n"); return nullptr;
    }
    IASIO* a = nullptr;
    if (FAILED(CoCreateInstance(c, nullptr, CLSCTX_INPROC_SERVER, c, (void**)&a)) || !a) { printf("ASIO: create failed\n"); return nullptr; }
    if (!a->init(wnd)) { char e[128] = {}; a->getErrorMessage(e); printf("ASIO: init failed: %s\n", e); a->Release(); return nullptr; }
    long mn, mx, pref, gran; a->getBufferSize(&mn, &mx, &pref, &gran);
    char want[16] = {};
    if (GetEnvironmentVariableA("HARNESS_ASIO_RATE", want, 16) && a->canSampleRate(atof(want)) == 0) a->setSampleRate(atof(want));
    a->getSampleRate(&g_asioRate);
    for (int i = 0; i < 2; ++i) { g_asioBufs[i] = {}; g_asioBufs[i].channelNum = i; }
    if (a->createBuffers(g_asioBufs, 2, pref, &g_asioCb)) { printf("ASIO: createBuffers failed\n"); a->Release(); return nullptr; }
    ASIOChannelInfo ci = {}; ci.channel = 0; a->getChannelInfo(&ci);
    g_asioSize = pref; g_asioType = ci.type;
    long inL, outL; a->getLatencies(&inL, &outL);
    g_asioOutLat = outL;
    a->start();
    printf("ASIO: %ls, %ld frames @ %.0f Hz, type %ld, out latency %ld\n", name, pref, g_asioRate, g_asioType, outL);
    return a;
}

// ---- the game's recorder ----------------------------------------------------------------------

static IMFTransform* make_aac(IMFMediaType** outType) {
    IMFTransform* t = nullptr;
    CHECK(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)));
    IMFMediaType *in = nullptr, *out = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2); in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, RATE * 4);
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2); out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    if (FAILED(t->SetInputType(0, in, 0)) || FAILED(t->SetOutputType(0, out, 0))) { CHECK(t->SetOutputType(0, out, 0)); CHECK(t->SetInputType(0, in, 0)); }
    CHECK(t->GetOutputCurrentType(0, outType));
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    in->Release(); out->Release();
    return t;
}

struct FinalizeCb : IMFAsyncCallback {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IMFAsyncResult* result = nullptr;
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IMFAsyncCallback)) { *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* r) override { result = r; r->AddRef(); SetEvent(ev); return S_OK; }
};

struct Recorder {
    void* enc = nullptr; IDirect3DSurface9* surf = nullptr;
    NV_ENC_REGISTER_RESOURCE rr = {}; NV_ENC_OUTPUT_PTR bs = nullptr;
    IMFTransform* aac = nullptr; IMFMediaType *vt = nullptr, *at = nullptr;
    IMFByteStream* bstream = nullptr; IMFMediaSink* sink = nullptr; IMFPresentationClock* clock = nullptr;
    IMFStreamSink *vs = nullptr, *as = nullptr;
    int frames = 0; int64_t audioN = 0;
    int rw = W, rh = H; bool alt = false;   // alt: a second, smaller encoder with different content

    void begin(IDirect3DDevice9* dev, const std::wstring& path) {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {}; op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; op.device = dev; op.apiVersion = NVENCAPI_VERSION;
        NVCHECK(fl.nvEncOpenEncodeSessionEx(&op, &enc));
        NV_ENC_PRESET_CONFIG pc = {}; pc.version = NV_ENC_PRESET_CONFIG_VER; pc.presetCfg.version = NV_ENC_CONFIG_VER;
        NVCHECK(fl.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_LOW_LATENCY, &pc));
        NV_ENC_CONFIG cfg = pc.presetCfg;
        cfg.gopLength = NVENC_INFINITE_GOPLENGTH; cfg.frameIntervalP = 1;
        cfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP; cfg.rcParams.constQP = { 36, 39, 33 };
        NV_ENC_INITIALIZE_PARAMS ip = {}; ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
        ip.encodeGUID = NV_ENC_CODEC_H264_GUID; ip.presetGUID = NV_ENC_PRESET_P4_GUID; ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        ip.encodeWidth = rw; ip.encodeHeight = rh; ip.darWidth = rw; ip.darHeight = rh;
        ip.frameRateNum = REC_FPS; ip.frameRateDen = 1; ip.enablePTD = 1; ip.encodeConfig = &cfg;
        NVCHECK(fl.nvEncInitializeEncoder(enc, &ip));
        std::vector<uint8_t> seq(1024); uint32_t seqSize = 0;
        NV_ENC_SEQUENCE_PARAM_PAYLOAD sp = {}; sp.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
        sp.inBufferSize = (uint32_t)seq.size(); sp.spsppsBuffer = seq.data(); sp.outSPSPPSPayloadSize = &seqSize;
        NVCHECK(fl.nvEncGetSequenceParams(enc, &sp));

        CHECK(dev->CreateRenderTarget(rw, rh, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &surf, nullptr));
        rr.version = NV_ENC_REGISTER_RESOURCE_VER; rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.resourceToRegister = surf; rr.width = rw; rr.height = rh; rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        NVCHECK(fl.nvEncRegisterResource(enc, &rr));
        NV_ENC_CREATE_BITSTREAM_BUFFER bb = {}; bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        NVCHECK(fl.nvEncCreateBitstreamBuffer(enc, &bb)); bs = bb.bitstreamBuffer;

        aac = make_aac(&at);
        CHECK(MFCreateMediaType(&vt));
        vt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); vt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(vt, MF_MT_FRAME_SIZE, rw, rh); MFSetAttributeRatio(vt, MF_MT_FRAME_RATE, REC_FPS, 1);
        MFSetAttributeRatio(vt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        vt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); vt->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
        vt->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seq.data(), seqSize);
        CHECK(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, path.c_str(), &bstream));
        CHECK(MFCreateMPEG4MediaSink(bstream, vt, at, &sink));
        IMFPresentationTimeSource* ts = nullptr;
        CHECK(MFCreatePresentationClock(&clock)); CHECK(MFCreateSystemTimeSource(&ts));
        clock->SetTimeSource(ts); ts->Release();
        CHECK(sink->SetPresentationClock(clock)); CHECK(clock->Start(0));
        DWORD n = 0; sink->GetStreamSinkCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            IMFStreamSink* ss = nullptr; sink->GetStreamSinkByIndex(i, &ss);
            IMFMediaTypeHandler* h = nullptr; ss->GetMediaTypeHandler(&h);
            GUID major; h->GetMajorType(&major); h->Release();
            (major == MFMediaType_Video ? vs : as) = ss;
        }
    }

    // Like the game: the recorder surface gets the frame without the hidden elements.
    void frame(IDirect3DDevice9* dev, int f) {
        if (alt) dev->ColorFill(surf, nullptr, D3DCOLOR_ARGB(255, 0, 255, 0));
        else draw_scene(dev, surf, f, false);
        NV_ENC_MAP_INPUT_RESOURCE mp = {}; mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER; mp.registeredResource = rr.registeredResource;
        NVCHECK(fl.nvEncMapInputResource(enc, &mp));
        NV_ENC_PIC_PARAMS pic = {}; pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = rw; pic.inputHeight = rh; pic.inputBuffer = mp.mappedResource;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB; pic.outputBitstream = bs; pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = frames;
        NVCHECK(fl.nvEncEncodePicture(enc, &pic));
        NV_ENC_LOCK_BITSTREAM lb = {}; lb.version = NV_ENC_LOCK_BITSTREAM_VER; lb.outputBitstream = bs;
        NVCHECK(fl.nvEncLockBitstream(enc, &lb));
        IMFMediaBuffer* b = nullptr; MFCreateMemoryBuffer(lb.bitstreamSizeInBytes, &b);
        BYTE* p = nullptr; b->Lock(&p, nullptr, nullptr); memcpy(p, lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes);
        b->Unlock(); b->SetCurrentLength(lb.bitstreamSizeInBytes);
        bool key = lb.pictureType == NV_ENC_PIC_TYPE_IDR;
        fl.nvEncUnlockBitstream(enc, bs);
        fl.nvEncUnmapInputResource(enc, mp.mappedResource);
        IMFSample* s = nullptr; MFCreateSample(&s); s->AddBuffer(b); b->Release();
        s->SetSampleTime((int64_t)frames * 10000000 / REC_FPS); s->SetSampleDuration(10000000 / REC_FPS);
        if (key) s->SetUINT32(MFSampleExtension_CleanPoint, 1);
        CHECK(vs->ProcessSample(s)); s->Release();
        ++frames;
        while (audioN * 1024LL * REC_FPS <= (int64_t)frames * RATE) {
            IMFMediaBuffer* ab = nullptr; MFCreateMemoryBuffer(4096, &ab);
            BYTE* ap = nullptr; ab->Lock(&ap, nullptr, nullptr);
            for (int i = 0; i < 1024; ++i) { int16_t v = (int16_t)(6000 * sin(2 * 3.14159265 * 330 * (audioN * 1024 + i) / RATE)); ((int16_t*)ap)[i * 2] = ((int16_t*)ap)[i * 2 + 1] = v; }
            ab->Unlock(); ab->SetCurrentLength(4096);
            IMFSample* in = nullptr; MFCreateSample(&in); in->AddBuffer(ab); ab->Release();
            in->SetSampleTime(audioN * 1024 * 10000000LL / RATE); in->SetSampleDuration(1024 * 10000000LL / RATE);
            CHECK(aac->ProcessInput(0, in, 0)); in->Release(); ++audioN;
            for (;;) {
                MFT_OUTPUT_STREAM_INFO si = {}; aac->GetOutputStreamInfo(0, &si);
                IMFSample* o = nullptr; IMFMediaBuffer* ob = nullptr; MFCreateSample(&o);
                MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 8192, &ob); o->AddBuffer(ob); ob->Release();
                MFT_OUTPUT_DATA_BUFFER od = {}; od.pSample = o; DWORD st = 0;
                HRESULT hr = aac->ProcessOutput(0, 1, &od, &st);
                if (od.pEvents) od.pEvents->Release();
                if (FAILED(hr)) { o->Release(); break; }
                CHECK(as->ProcessSample(o)); o->Release();
            }
        }
    }

    void end() {
        IMFFinalizableMediaSink* fin = nullptr; CHECK(sink->QueryInterface(IID_PPV_ARGS(&fin)));
        FinalizeCb cb; CHECK(fin->BeginFinalize(&cb, nullptr)); WaitForSingleObject(cb.ev, 10000); CHECK(fin->EndFinalize(cb.result));
        clock->Stop(); sink->Shutdown(); bstream->Close();
        fin->Release(); vs->Release(); as->Release(); sink->Release(); clock->Release(); bstream->Release();
        vt->Release(); at->Release(); aac->Release();
        fl.nvEncDestroyBitstreamBuffer(enc, bs); fl.nvEncUnregisterResource(enc, rr.registeredResource);
        fl.nvEncDestroyEncoder(enc); surf->Release();
        printf("recorder: %d frames written\n", frames);
    }
};

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) { printf("usage: harness <proxy dll> <out prefix> <none|wasapi|asio> [menu s] [record s] [delay ms]\n"); return 2; }
    std::wstring audio = argv[3];
    int menuS = argc > 4 ? _wtoi(argv[4]) : 3, recS = argc > 5 ? _wtoi(argv[5]) : 3, delay = argc > 6 ? _wtoi(argv[6]) : 1500;

    CHECK(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    CHECK(MFStartup(MF_VERSION));
    HWND wnd = CreateWindowExW(0, L"STATIC", L"harness", WS_POPUP, 0, 0, W, H, nullptr, nullptr, nullptr, nullptr);

    // The device exists before the proxy loads, like a game that loaded it as a dependency.
    IDirect3D9Ex* d3d = nullptr;
    CHECK(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d));
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferWidth = W; pp.BackBufferHeight = H;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9Ex* dev = nullptr;
    CHECK(d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
          D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, nullptr, &dev));
    IDirect3DSwapChain9* sc = nullptr;
    CHECK(dev->GetSwapChain(0, &sc));

    Wasapi was;
    if (audio == L"wasapi-early" && !was.start()) printf("WASAPI start failed\n");   // opened before our hooks

    HMODULE m = LoadLibraryW(argv[1]);
    if (!m) { printf("LoadLibrary(%ls) failed %lu\n", argv[1], GetLastError()); return 1; }
    auto create = (NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*))GetProcAddress(m, "NvEncodeAPICreateInstance");
    fl.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVCHECK(create(&fl));   // waits for the proxy's init (hooks installed)
    CHECK(dev->ResetEx(&pp, nullptr));   // goes through the proxy's Reset hook

    IASIO* asio = nullptr;
    if (audio == L"wasapi" && !was.start()) printf("WASAPI start failed\n");
    if (audio == L"asio") asio = start_asio(wnd);

    LARGE_INTEGER freq, t0, now; QueryPerformanceFrequency(&freq);
    Sleep(delay);
    QueryPerformanceCounter(&t0);
    int total = (2 * menuS + recS) * HZ, recFrom = menuS * HZ, recTo = (menuS + recS) * HZ;
    IDirect3DSurface9* bb = nullptr;
    // Like the game: two recorder encoders per song. The second is smaller, green, and submits
    // last each frame, so following the most recent submitter would show green.
    Recorder rec, rec2;
    rec2.rw = 640; rec2.rh = 360; rec2.alt = true;
    int stallAt = recTo + HZ / 2;           // a 1.5 s "loading screen" with no presents
    g_flashT0 = t0.QuadPart; g_flashFreq = freq.QuadPart;
    for (int f = 0; f < total; ++f) {
        if (f == recFrom) {
            rec.begin(dev, std::wstring(argv[2]) + L"-rec.mp4");
            rec2.begin(dev, std::wstring(argv[2]) + L"-rec2.mp4");
            printf("recorders started at %.2f s\n", (double)f / HZ);
        }
        if (f >= recFrom && f < recTo && (f % (HZ / REC_FPS)) == 0) { rec.frame(dev, f); rec2.frame(dev, f); }
        if (f == recTo) { rec.end(); rec2.end(); printf("recorders stopped at %.2f s\n", (double)f / HZ); }
        if (f == stallAt) {
            printf("stall (no presents) at %.2f s\n", (double)f / HZ);
            Sleep(1500);
            t0.QuadPart += freq.QuadPart * 3 / 2;
            g_flashT0 = t0.QuadPart;
        }
        sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
        draw_scene(dev, bb, f, true);
        if (f % HZ < 2) {    // sync flash once a second (2 frames, so 60 fps capture sees it), matched by an audio tick
            RECT r = { 0, 0, 320, 180 };
            dev->ColorFill(bb, &r, D3DCOLOR_ARGB(255, 255, 255, 255));
        }
        bb->Release();
        sc->Present(nullptr, nullptr, nullptr, nullptr, 0);
        if (was.ac) was.fill();
        int64_t due = t0.QuadPart + (int64_t)(f + 1) * freq.QuadPart / HZ;
        for (;;) { QueryPerformanceCounter(&now); if (now.QuadPart >= due) break; Sleep(0); }
    }
    if (asio) { asio->stop(); asio->disposeBuffers(); asio->Release(); }
    if (was.ac) was.ac->Stop();
    Sleep(800);   // let the proxy flush to viewers
    printf("DONE\n");
    return 0;
}
