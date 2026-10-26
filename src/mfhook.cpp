// Taps the game's own MP4 recording: MFCreateMPEG4MediaSink is redirected through the IAT so we
// learn each recording's media types, and IMFStreamSink::ProcessSample is hooked in the stream
// sinks' vtable so every compressed sample (H.264 from NVENC, AAC from the game's audio encoder)
// is copied to the output worker before the MP4 sink writes it. The game's file is untouched.
#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>

#include <mutex>
#include <unordered_map>

#include "common.h"
#include "output.h"

typedef HRESULT (STDAPICALLTYPE* PFN_CreateMP4Sink)(IMFByteStream*, IMFMediaType*, IMFMediaType*, IMFMediaSink**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ProcessSample)(IMFStreamSink*, IMFSample*);

static const int kProcessSampleSlot = 10;  // IUnknown(3) + IMFMediaEventGenerator(4) + GetMediaSink,GetIdentifier,GetMediaTypeHandler

struct StreamTag { ItemKind kind; uint32_t session; };

static PFN_CreateMP4Sink g_realCreate;
static std::mutex        g_mtx;
static std::unordered_map<IMFStreamSink*, StreamTag> g_streams;
static std::unordered_map<void*, PFN_ProcessSample>  g_origProcess;  // vtable -> original
static uint32_t          g_session;
static UINT64            g_sessionArea;      // frame area of the recording being forwarded
static int64_t           g_sessionActive;    // its creation or latest sample (qpc_100ns)

static int freq_index(UINT32 rate) {
    static const UINT32 t[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    for (int i = 0; i < 13; ++i) if (t[i] == rate) return i;
    return 3;
}

static void describe_audio(IMFMediaType* t, AudioInfo& a) {
    GUID sub = GUID_NULL;
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    if (sub != MFAudioFormat_AAC) { logf("audio subtype is not AAC, audio will not be forwarded"); return; }
    UINT32 payload = MFGetAttributeUINT32(t, MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (payload > 1) { logf("AAC payload type %u unsupported, audio will not be forwarded", payload); return; }
    a.present    = true;
    a.adts       = payload == 1;
    a.objectType = 2;
    a.freqIndex  = freq_index(MFGetAttributeUINT32(t, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
    a.channels   = (int)MFGetAttributeUINT32(t, MF_MT_AUDIO_NUM_CHANNELS, 2);

    // MF_MT_USER_DATA = HEAACWAVEINFO fields after WAVEFORMATEX (12 bytes) + AudioSpecificConfig.
    UINT32 sz = 0;
    if (SUCCEEDED(t->GetBlobSize(MF_MT_USER_DATA, &sz)) && sz >= 14) {
        std::vector<uint8_t> ud(sz);
        if (SUCCEEDED(t->GetBlob(MF_MT_USER_DATA, ud.data(), sz, nullptr))) {
            int ot = ud[12] >> 3;
            int fi = ((ud[12] & 7) << 1) | (ud[13] >> 7);
            int ch = (ud[13] >> 3) & 0xF;
            if (ot >= 1 && ot <= 4) a.objectType = ot;
            if (fi < 13) a.freqIndex = fi;
            if (ch >= 1 && ch <= 7) a.channels = ch;
        }
    }
    logf("audio: AAC %s object=%d freqIndex=%d channels=%d", a.adts ? "ADTS" : "raw",
         a.objectType, a.freqIndex, a.channels);
}

static void tap(IMFStreamSink* self, IMFSample* s) {
    StreamTag tag;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_streams.find(self);
        if (it == g_streams.end()) return;
        tag = it->second;
        if (tag.session == g_session) g_sessionActive = qpc_100ns();
    }
    Item it;
    it.kind = tag.kind;
    it.session = tag.session;
    it.wall = qpc_100ns();
    LONGLONG t = 0;
    if (FAILED(s->GetSampleTime(&t))) return;
    it.pts = t;
    UINT64 d = 0;
    it.dts = SUCCEEDED(s->GetUINT64(MFSampleExtension_DecodeTimestamp, &d)) ? (int64_t)d : t;
    it.key = MFGetAttributeUINT32(s, MFSampleExtension_CleanPoint, 0) != 0;

    DWORD nb = 0;
    s->GetBufferCount(&nb);
    for (DWORD i = 0; i < nb; ++i) {
        IMFMediaBuffer* b = nullptr;
        if (FAILED(s->GetBufferByIndex(i, &b))) continue;
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
            it.data.insert(it.data.end(), p, p + len);
            b->Unlock();
        }
        b->Release();
    }
    if (!it.data.empty()) output_push(std::move(it));
}

static HRESULT STDMETHODCALLTYPE Hook_ProcessSample(IMFStreamSink* self, IMFSample* s) {
    PFN_ProcessSample orig;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        orig = g_origProcess[*(void**)self];
    }
    if (s && output_active()) tap(self, s);
    return orig ? orig(self, s) : E_FAIL;
}

static void hook_vtable(IMFStreamSink* ss) {
    void** vtbl = *(void***)ss;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_origProcess.count(vtbl)) return;
    g_origProcess[vtbl] = (PFN_ProcessSample)vtbl[kProcessSampleSlot];
    DWORD old;
    VirtualProtect(&vtbl[kProcessSampleSlot], sizeof(void*), PAGE_READWRITE, &old);
    vtbl[kProcessSampleSlot] = (void*)&Hook_ProcessSample;
    VirtualProtect(&vtbl[kProcessSampleSlot], sizeof(void*), old, &old);
}

static void on_sink_created(IMFMediaSink* sink, IMFMediaType* vt, IMFMediaType* at) {
    UINT64 fs = 0;
    if (vt) vt->GetUINT64(MF_MT_FRAME_SIZE, &fs);
    UINT64 area = (fs >> 32) * (fs & 0xFFFFFFFF);
    Item it;
    it.kind = ItemKind::Session;
    {
        // The game writes two recordings per song; forward only the largest one.
        std::lock_guard<std::mutex> lk(g_mtx);
        int64_t now = qpc_100ns();
        if (g_session && area < g_sessionArea && now - g_sessionActive < 20000000) {
            logf("ignoring a smaller recording (%ux%u) made alongside the current one",
                 (UINT32)(fs >> 32), (UINT32)fs);
            return;
        }
        it.session = ++g_session;
        g_sessionArea = area;
        g_sessionActive = now;
    }
    UINT32 sz = 0;
    if (vt && SUCCEEDED(vt->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz) {
        it.info.seqHeader.resize(sz);
        vt->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, it.info.seqHeader.data(), sz, nullptr);
    }
    if (at) describe_audio(at, it.info.audio);

    DWORD n = 0;
    sink->GetStreamSinkCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        IMFStreamSink* ss = nullptr;
        if (FAILED(sink->GetStreamSinkByIndex(i, &ss))) continue;
        GUID major = GUID_NULL;
        IMFMediaTypeHandler* h = nullptr;
        if (SUCCEEDED(ss->GetMediaTypeHandler(&h))) {
            h->GetMajorType(&major);
            if (major == MFMediaType_Audio && !it.info.audio.present) {
                IMFMediaType* cur = nullptr;
                if (SUCCEEDED(h->GetCurrentMediaType(&cur))) { describe_audio(cur, it.info.audio); cur->Release(); }
            }
            h->Release();
        }
        if (major == MFMediaType_Video || major == MFMediaType_Audio) {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_streams[ss] = { major == MFMediaType_Video ? ItemKind::Video : ItemKind::Audio, it.session };
            }
            hook_vtable(ss);
        }
        ss->Release();
    }
    logf("recording #%u started: %ux%u, %lu streams, seqhdr %u bytes, audio %s", it.session,
         (UINT32)(fs >> 32), (UINT32)fs, n, (UINT32)it.info.seqHeader.size(),
         it.info.audio.present ? "yes" : "no");
    output_push(std::move(it));
}

static HRESULT STDAPICALLTYPE Hook_CreateMP4Sink(IMFByteStream* bs, IMFMediaType* vt, IMFMediaType* at,
                                                 IMFMediaSink** out) {
    HRESULT hr = g_realCreate(bs, vt, at, out);
    if (SUCCEEDED(hr) && out && *out) on_sink_created(*out, vt, at);
    return hr;
}

void install_mf_hook() {
    HMODULE mf = LoadLibraryW(L"mf.dll");
    g_realCreate = mf ? (PFN_CreateMP4Sink)GetProcAddress(mf, "MFCreateMPEG4MediaSink") : nullptr;
    if (!g_realCreate) { logf("MFCreateMPEG4MediaSink not found, recording tap disabled"); return; }
    if (!iat_hook_all((void*)g_realCreate, (void*)&Hook_CreateMP4Sink, "MFCreateMPEG4MediaSink"))
        logf("no module imports MFCreateMPEG4MediaSink, recording tap disabled");
}
