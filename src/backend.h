#pragma once
#include <d3d9.h>
#include <cstdint>
#include <vector>

struct Bitstream { std::vector<uint8_t> nal; uint64_t pts = 0; bool keyframe = false; };

struct IEncoderBackend {
    virtual ~IEncoderBackend() {}
    virtual bool init(IDirect3DDevice9* dev, int w, int h, int fpsN, int fpsD,
                      int qpI, int qpP, int qpB, int idrPeriod) = 0;
    virtual bool encode(IDirect3DSurface9* surf, uint64_t pts, bool forceIDR, Bitstream& out) = 0;
    virtual bool drain_remaining(Bitstream& out) = 0;
    virtual bool get_header(std::vector<uint8_t>& out) = 0;
};

IEncoderBackend* create_backend();
bool probe_primary_available();

inline bool readback_bgra(IDirect3DDevice9* dev, IDirect3DSurface9* surf,
                          IDirect3DSurface9** staging,
                          std::vector<uint8_t>& px, int& w, int& h, int& pitch) {
    D3DSURFACE_DESC d; if (FAILED(surf->GetDesc(&d))) return false;
    w = (int)d.Width; h = (int)d.Height;
    if (!*staging) {
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format,
                    D3DPOOL_SYSTEMMEM, staging, nullptr))) return false;
    }
    if (FAILED(dev->GetRenderTargetData(surf, *staging))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED((*staging)->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    pitch = lr.Pitch; px.resize((size_t)lr.Pitch * h);
    memcpy(px.data(), lr.pBits, px.size());
    (*staging)->UnlockRect();
    return true;
}
