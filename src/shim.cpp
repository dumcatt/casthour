#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include <mutex>
#include <vector>

#include "nvEncodeAPI.h"
#include "backend.h"

static const GUID IID_WrappedIDirect3DDevice9 =
    { 0x6dec0d40, 0x1339, 0x4bda, { 0xa5, 0xf2, 0x22, 0x31, 0xd4, 0x01, 0x0f, 0xd1 } };

static IDirect3DDevice9* unwrap_device(void* dev) {
    if (!dev) return nullptr;
    void* probe = nullptr;
    if (SUCCEEDED(reinterpret_cast<IUnknown*>(dev)->QueryInterface(
            IID_WrappedIDirect3DDevice9, &probe))) {
        reinterpret_cast<IUnknown*>(probe)->Release();
        return *reinterpret_cast<IDirect3DDevice9**>(
            reinterpret_cast<char*>(dev) + 16);
    }
    return reinterpret_cast<IDirect3DDevice9*>(dev);
}

static int g_amfOK = -1;

static bool amf_available() {
    if (g_amfOK < 0) g_amfOK = probe_primary_available() ? 1 : 0;
    return g_amfOK == 1;
}

struct Session {
    IDirect3DDevice9*        d3d9 = nullptr;
    int                      w = 0, h = 0, fpsN = 30, fpsD = 1;
    int                      qpI = 33, qpP = 36, qpB = 33;
    IEncoderBackend*         enc = nullptr;
    std::mutex               mtx;

    struct Reg { IDirect3DSurface9* surf; NV_ENC_BUFFER_FORMAT fmt; };
};

static NVENCSTATUS NVENCAPI Nv_OpenEncodeSessionEx(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* p,
                                                   void** enc) {
    auto* s = new Session();
    s->d3d9 = unwrap_device(p->device);
    if (s->d3d9) s->d3d9->AddRef();
    s->enc = create_backend();
    *enc = s;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_InitializeEncoder(void* enc, NV_ENC_INITIALIZE_PARAMS* ip) {
    auto* s = (Session*)enc;
    s->w = ip->encodeWidth; s->h = ip->encodeHeight;
    if (ip->frameRateNum) { s->fpsN = ip->frameRateNum; s->fpsD = ip->frameRateDen ? ip->frameRateDen : 1; }
    if (ip->encodeConfig && ip->encodeConfig->rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
        s->qpI = ip->encodeConfig->rcParams.constQP.qpIntra;
        s->qpP = ip->encodeConfig->rcParams.constQP.qpInterP;
        s->qpB = ip->encodeConfig->rcParams.constQP.qpInterB;
    }
    uint32_t gop = ip->encodeConfig ? ip->encodeConfig->gopLength : 0;
    int fps = s->fpsN / (s->fpsD ? s->fpsD : 1); if (fps < 1) fps = 30;
    int idrPeriod = (gop != 0 && gop != NVENC_INFINITE_GOPLENGTH && gop < 100000)
                        ? (int)gop : (fps * 2 < 60 ? 60 : fps * 2);
    bool ok = s->enc && s->enc->init(s->d3d9, s->w, s->h, s->fpsN, s->fpsD,
                                     s->qpI, s->qpP, s->qpB, idrPeriod);
    return ok ? NV_ENC_SUCCESS : NV_ENC_ERR_GENERIC;
}

static NVENCSTATUS NVENCAPI Nv_RegisterResource(void* enc, NV_ENC_REGISTER_RESOURCE* r) {
    auto* reg = new Session::Reg{ (IDirect3DSurface9*)r->resourceToRegister, r->bufferFormat };
    r->registeredResource = reg;
    return NV_ENC_SUCCESS;
}
static NVENCSTATUS NVENCAPI Nv_UnregisterResource(void* enc, NV_ENC_REGISTERED_PTR p) {
    delete (Session::Reg*)p; return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_MapInputResource(void* enc, NV_ENC_MAP_INPUT_RESOURCE* m) {
    m->mappedResource = m->registeredResource;
    return NV_ENC_SUCCESS;
}
static NVENCSTATUS NVENCAPI Nv_UnmapInputResource(void*, NV_ENC_INPUT_PTR) { return NV_ENC_SUCCESS; }

static NVENCSTATUS NVENCAPI Nv_CreateBitstreamBuffer(void*, NV_ENC_CREATE_BITSTREAM_BUFFER* b) {
    b->bitstreamBuffer = new Bitstream(); return NV_ENC_SUCCESS;
}
static NVENCSTATUS NVENCAPI Nv_DestroyBitstreamBuffer(void*, NV_ENC_OUTPUT_PTR p) {
    delete (Bitstream*)p; return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_EncodePicture(void* enc, NV_ENC_PIC_PARAMS* pp) {
    auto* s = (Session*)enc; std::lock_guard<std::mutex> lk(s->mtx);
    auto* out = (Bitstream*)pp->outputBitstream;
    if (pp->encodePicFlags & NV_ENC_PIC_FLAG_EOS) {
        if (out) { out->nal.clear(); s->enc->drain_remaining(*out); }
        if (pp->completionEvent) SetEvent(pp->completionEvent);
        return NV_ENC_SUCCESS;
    }
    auto* reg = (Session::Reg*)pp->inputBuffer;
    if (!reg || !out) return NV_ENC_ERR_INVALID_PARAM;
    bool forceIDR = (pp->encodePicFlags & (NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_FORCEINTRA)) != 0;
    if (!s->enc->encode(reg->surf, pp->inputTimeStamp, forceIDR, *out)) return NV_ENC_ERR_GENERIC;
    if (pp->completionEvent) SetEvent(pp->completionEvent);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_LockBitstream(void*, NV_ENC_LOCK_BITSTREAM* l) {
    auto* bs = (Bitstream*)l->outputBitstream;
    l->bitstreamBufferPtr  = bs->nal.data();
    l->bitstreamSizeInBytes = (uint32_t)bs->nal.size();
    l->outputTimeStamp     = bs->pts;
    l->pictureType         = bs->keyframe ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
    return NV_ENC_SUCCESS;
}
static NVENCSTATUS NVENCAPI Nv_UnlockBitstream(void*, NV_ENC_OUTPUT_PTR) { return NV_ENC_SUCCESS; }

static NVENCSTATUS NVENCAPI Nv_GetEncodeGUIDs(void*, GUID* g, uint32_t cap, uint32_t* cnt) {
    static const GUID H264 = NV_ENC_CODEC_H264_GUID;
    if (g && cap >= 1) g[0] = H264;
    if (cnt) *cnt = 1;
    return NV_ENC_SUCCESS;
}
static NVENCSTATUS NVENCAPI Nv_GetInputFormats(void*, GUID, NV_ENC_BUFFER_FORMAT* f,
                                               uint32_t cap, uint32_t* cnt) {
    if (f && cap >= 1) f[0] = NV_ENC_BUFFER_FORMAT_ARGB;
    if (cnt) *cnt = 1;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_GetEncodePresetConfig(void*, GUID, GUID, NV_ENC_PRESET_CONFIG* pc) {
    if (!pc) return NV_ENC_ERR_INVALID_PTR;
    pc->version = NV_ENC_PRESET_CONFIG_VER;
    pc->presetCfg.version = NV_ENC_CONFIG_VER;
    return NV_ENC_SUCCESS;
}
static NVENCSTATUS NVENCAPI Nv_GetEncodePresetConfigEx(void*, GUID, GUID, NV_ENC_TUNING_INFO,
                                                       NV_ENC_PRESET_CONFIG* pc) {
    if (!pc) return NV_ENC_ERR_INVALID_PTR;
    pc->version = NV_ENC_PRESET_CONFIG_VER;
    pc->presetCfg.version = NV_ENC_CONFIG_VER;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_GetSequenceParams(void* enc, NV_ENC_SEQUENCE_PARAM_PAYLOAD* p) {
    auto* s = (Session*)enc;
    if (!p || !p->spsppsBuffer || !p->outSPSPPSPayloadSize) return NV_ENC_ERR_INVALID_PARAM;
    std::vector<uint8_t> hdr;
    if (!s->enc || !s->enc->get_header(hdr) || hdr.empty())
        return NV_ENC_ERR_GENERIC;
    if (hdr.size() > p->inBufferSize) return NV_ENC_ERR_INVALID_PARAM;
    memcpy(p->spsppsBuffer, hdr.data(), hdr.size());
    *p->outSPSPPSPayloadSize = (uint32_t)hdr.size();
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_DestroyEncoder(void* enc) {
    auto* s = (Session*)enc;
    if (s->d3d9) s->d3d9->Release();
    delete s->enc; delete s;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI Nv_ok(void*) { return NV_ENC_SUCCESS; }
static const char* NVENCAPI Nv_errstr(void*) { return ""; }

NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t* version) {
    if (version) *version = 0;
    if (!amf_available()) return NV_ENC_ERR_NO_ENCODE_DEVICE;
    if (version) *version = (9 << 4) | 1;
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST* fl) {
    if (!fl) return NV_ENC_ERR_INVALID_PTR;
    if (!amf_available()) return NV_ENC_ERR_NO_ENCODE_DEVICE;

    for (void** p = (void**)&fl->nvEncOpenEncodeSession; p < (void**)&fl->reserved2[0]; ++p)
        *p = (void*)&Nv_ok;
    fl->nvEncGetLastErrorString = Nv_errstr;

    fl->nvEncOpenEncodeSessionEx = Nv_OpenEncodeSessionEx;
    fl->nvEncInitializeEncoder   = Nv_InitializeEncoder;
    fl->nvEncRegisterResource    = Nv_RegisterResource;
    fl->nvEncUnregisterResource  = Nv_UnregisterResource;
    fl->nvEncMapInputResource    = Nv_MapInputResource;
    fl->nvEncUnmapInputResource  = Nv_UnmapInputResource;
    fl->nvEncCreateBitstreamBuffer  = Nv_CreateBitstreamBuffer;
    fl->nvEncDestroyBitstreamBuffer = Nv_DestroyBitstreamBuffer;
    fl->nvEncEncodePicture       = Nv_EncodePicture;
    fl->nvEncLockBitstream       = Nv_LockBitstream;
    fl->nvEncUnlockBitstream     = Nv_UnlockBitstream;
    fl->nvEncGetEncodeGUIDs      = Nv_GetEncodeGUIDs;
    fl->nvEncGetInputFormats     = Nv_GetInputFormats;
    fl->nvEncGetEncodePresetConfig   = Nv_GetEncodePresetConfig;
    fl->nvEncGetEncodePresetConfigEx = Nv_GetEncodePresetConfigEx;
    fl->nvEncGetSequenceParams   = Nv_GetSequenceParams;
    fl->nvEncDestroyEncoder      = Nv_DestroyEncoder;
    return NV_ENC_SUCCESS;
}

#if defined(BACKEND_STUB)
struct StubBackend : IEncoderBackend {
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* staging = nullptr;
    bool init(IDirect3DDevice9* d, int, int, int, int, int, int, int, int) override {
        dev = d; return true;
    }
    bool encode(IDirect3DSurface9* surf, uint64_t pts, bool, Bitstream& o) override {
        std::vector<uint8_t> px; int w, h, pitch;
        readback_bgra(dev, surf, &staging, px, w, h, pitch);
        o.nal.clear(); o.pts = pts; o.keyframe = false; return true;
    }
    bool drain_remaining(Bitstream&) override { return false; }
    bool get_header(std::vector<uint8_t>&) override { return false; }
    ~StubBackend() override { if (staging) staging->Release(); }
};
IEncoderBackend* create_backend() { return new StubBackend(); }
bool probe_primary_available() { return true; }
#endif

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
