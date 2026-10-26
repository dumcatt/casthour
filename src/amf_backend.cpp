#if defined(BACKEND_AMF)

#include <d3d9.h>
#include <vector>
#include <cstdint>

#include "backend.h"

#include "public/common/AMFFactory.h"
#include "public/include/core/Context.h"
#include "public/include/core/Surface.h"
#include "public/include/components/Component.h"
#include "public/include/components/VideoEncoderVCE.h"

using namespace amf;

struct AmfBackend : IEncoderBackend {
    AMFContextPtr        context;
    AMFComponentPtr      encoder;
    int                  w = 0, h = 0;
    std::vector<uint8_t> header;
    IDirect3DDevice9*    m_dev = nullptr;
    IDirect3DSurface9*   m_staging = nullptr;
    int                  m_idrPeriod = 120;
    uint64_t             m_frame = 0;

    bool init(IDirect3DDevice9* dev, int width, int height, int fpsN, int fpsD,
              int qpI, int qpP, int qpB, int idrPeriod) override {
        w = width; h = height;
        m_dev = dev;
        m_idrPeriod = idrPeriod > 0 ? idrPeriod : 120;
        AMF_RESULT r;
        r = g_AMFFactory.Init();                 if (r!=AMF_OK) return false;
        AMFFactory* factory = g_AMFFactory.GetFactory();
        r = factory->CreateContext(&context);    if (r!=AMF_OK) return false;
        r = context->InitDX11(nullptr);          if (r!=AMF_OK) return false;

        r = factory->CreateComponent(context, AMFVideoEncoderVCE_AVC, &encoder);
        if (r!=AMF_OK) return false;

        encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
        encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                             AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP);
        encoder->SetProperty(AMF_VIDEO_ENCODER_QP_I, qpI);
        encoder->SetProperty(AMF_VIDEO_ENCODER_QP_P, qpP);
        encoder->SetProperty(AMF_VIDEO_ENCODER_QP_B, qpB);
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(w, h));
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fpsN, fpsD));
        encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
        encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        encoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64)1000);
        encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);
        encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64)idrPeriod);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING,
                             (amf_int64)(idrPeriod > 1000 ? 1000 : idrPeriod));

        r = encoder->Init(AMF_SURFACE_BGRA, w, h);
        if (r!=AMF_OK) return false;

        AMFVariant extra;
        if (encoder->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &extra) == AMF_OK) {
            AMFInterfacePtr iface(extra.ToInterface());
            AMFBufferPtr buf(iface);
            if (buf) {
                const uint8_t* p = (const uint8_t*)buf->GetNative();
                header.assign(p, p + buf->GetSize());
            }
        }
        return true;
    }

    bool get_header(std::vector<uint8_t>& out) override {
        if (header.empty()) return false;
        out = header;
        return true;
    }

    bool encode(IDirect3DSurface9* surf, uint64_t pts, bool forceIDR, Bitstream& out) override {
        out.nal.clear(); out.pts = pts;

        std::vector<uint8_t> px; int rw, rh, spitch;
        if (!readback_bgra(m_dev, surf, &m_staging, px, rw, rh, spitch)) return false;

        AMFSurfacePtr in;
        AMF_RESULT r = context->AllocSurface(AMF_MEMORY_HOST, AMF_SURFACE_BGRA, w, h, &in);
        if (r != AMF_OK) return false;
        AMFPlane* plane = in->GetPlaneAt(0);
        uint8_t* dst = (uint8_t*)plane->GetNative();
        int dpitch = plane->GetHPitch();
        int rowbytes = spitch < dpitch ? spitch : dpitch;
        for (int y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * dpitch, px.data() + (size_t)y * spitch, rowbytes);
        in->SetPts((amf_pts)pts);
        if (forceIDR || (m_frame % (uint64_t)m_idrPeriod) == 0) {
            in->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            in->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            in->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
        ++m_frame;

        r = encoder->SubmitInput(in);
        if (r == AMF_INPUT_FULL) {
            AMFDataPtr d; if (encoder->QueryOutput(&d) == AMF_OK && d) take_output(d, out);
            r = encoder->SubmitInput(in);
        }
        if (r != AMF_OK && r != AMF_NEED_MORE_INPUT) return false;

        AMFDataPtr data;
        if (encoder->QueryOutput(&data) == AMF_OK && data) take_output(data, out);
        return true;
    }

    void take_output(AMFDataPtr& data, Bitstream& out) {
        out.pts = (uint64_t)data->GetPts();
        amf_int64 t = 0;
        if (data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &t) == AMF_OK)
            out.keyframe = (t == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
        AMFBufferPtr buf(data);
        if (buf) {
            const uint8_t* p = (const uint8_t*)buf->GetNative();
            out.nal.insert(out.nal.end(), p, p + buf->GetSize());
        }
    }

    bool drain_remaining(Bitstream& out) override {
        encoder->Drain();
        AMFDataPtr data;
        while (encoder->QueryOutput(&data) == AMF_OK && data) { take_output(data, out); data = nullptr; }
        return !out.nal.empty();
    }

    ~AmfBackend() override {
        if (m_staging) m_staging->Release();
        if (encoder) encoder->Terminate();
        if (context) context->Terminate();
    }
};

IEncoderBackend* create_backend() { return new AmfBackend(); }

bool probe_primary_available() {
    if (g_AMFFactory.Init() != AMF_OK) return false;
    AMFContextPtr ctx;
    if (g_AMFFactory.GetFactory()->CreateContext(&ctx) != AMF_OK) return false;
    if (ctx->InitDX11(nullptr) != AMF_OK) { ctx->Terminate(); return false; }
    AMFComponentPtr comp;
    AMF_RESULT r = g_AMFFactory.GetFactory()->CreateComponent(ctx, AMFVideoEncoderVCE_AVC, &comp);
    if (comp) comp->Terminate();
    ctx->Terminate();
    return r == AMF_OK;
}

#endif
