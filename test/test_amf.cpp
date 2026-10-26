// Drives the shim like the game does, to validate the AMF backend on real AMD hardware.
// Build: cl /EHsc /std:c++17 /I ..\src test_amf.cpp d3d9.lib
// Run in a dir containing the AMF-built nvEncodeAPI64.dll.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <vector>
#include "nvEncodeAPI.h"

#define CK(x) do{ NVENCSTATUS _s=(x); printf("%-42s = %d %s\n", #x, _s, _s?"FAIL":"ok"); if(_s){} }while(0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    IDirect3D9Ex* d3d = nullptr;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d)) || !d3d) { printf("Create9Ex failed\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    IDirect3DDevice9Ex* devEx = nullptr;
    HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
                    D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED, &pp, nullptr, &devEx);
    IDirect3DDevice9* dev = devEx;
    printf("CreateDeviceEx hr=0x%08lX dev=%p\n", hr, (void*)dev);
    if (!dev) return 1;

    HMODULE m = LoadLibraryA("nvEncodeAPI64.dll");
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto CreateInstance = (NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*))
        GetProcAddress(m, "NvEncodeAPICreateInstance");
    NV_ENCODE_API_FUNCTION_LIST fl = {}; fl.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    CK(CreateInstance(&fl));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {};
    op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; op.device = dev; op.apiVersion = NVENCAPI_VERSION;
    void* enc = nullptr; CK(fl.nvEncOpenEncodeSessionEx(&op, &enc));
    if (!enc) { printf("no encoder\n"); return 1; }

    NV_ENC_CONFIG cfg = {}; cfg.version = NV_ENC_CONFIG_VER;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    cfg.rcParams.constQP = {36,39,33};
    NV_ENC_INITIALIZE_PARAMS ip = {}; ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.encodeWidth = 1920; ip.encodeHeight = 1080; ip.darWidth = 1920; ip.darHeight = 1080;
    ip.frameRateNum = 60; ip.frameRateDen = 1; ip.enablePTD = 1; ip.encodeConfig = &cfg;
    CK(fl.nvEncInitializeEncoder(enc, &ip));

    std::vector<uint8_t> sps(4096); uint32_t spsSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sp = {}; sp.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sp.inBufferSize = (uint32_t)sps.size(); sp.spsppsBuffer = sps.data(); sp.outSPSPPSPayloadSize = &spsSize;
    CK(fl.nvEncGetSequenceParams(enc, &sp));
    printf("SPS/PPS size = %u : ", spsSize);
    for (uint32_t i=0;i<spsSize && i<24;i++) printf("%02X ", sps[i]); printf("\n");

    IDirect3DSurface9* surf = nullptr;
    hr = dev->CreateRenderTarget(1920,1080,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&surf,nullptr);
    printf("CreateRenderTarget hr=0x%08lX surf=%p\n", hr, (void*)surf);
    if (surf) {
        NV_ENC_REGISTER_RESOURCE rr = {}; rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; rr.resourceToRegister = surf;
        rr.width=1920; rr.height=1080; rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        CK(fl.nvEncRegisterResource(enc, &rr));
        NV_ENC_MAP_INPUT_RESOURCE mp = {}; mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mp.registeredResource = rr.registeredResource; CK(fl.nvEncMapInputResource(enc, &mp));
        NV_ENC_CREATE_BITSTREAM_BUFFER bb = {}; bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        CK(fl.nvEncCreateBitstreamBuffer(enc, &bb));
        int got = 0, total = 0, idr = 0;
        for (int frame = 0; frame < 150; ++frame) {
            dev->ColorFill(surf, nullptr, D3DCOLOR_ARGB(255, frame*8 & 0xFF, 120, 220));
            NV_ENC_PIC_PARAMS pic = {}; pic.version = NV_ENC_PIC_PARAMS_VER;
            pic.inputWidth=1920; pic.inputHeight=1080; pic.inputBuffer=mp.mappedResource;
            pic.bufferFmt=NV_ENC_BUFFER_FORMAT_ARGB; pic.outputBitstream=bb.bitstreamBuffer;
            pic.pictureStruct=NV_ENC_PIC_STRUCT_FRAME;
            pic.inputTimeStamp = (uint64_t)frame * 1000;
            fl.nvEncEncodePicture(enc, &pic);
            NV_ENC_LOCK_BITSTREAM lb = {}; lb.version = NV_ENC_LOCK_BITSTREAM_VER;
            lb.outputBitstream = bb.bitstreamBuffer;
            fl.nvEncLockBitstream(enc, &lb);
            if (lb.bitstreamSizeInBytes) {
                if (got < 3) printf("frame %d: %u bytes  pts in=%llu out=%llu  picType=%u\n",
                    frame, lb.bitstreamSizeInBytes, (unsigned long long)pic.inputTimeStamp,
                    (unsigned long long)lb.outputTimeStamp, lb.pictureType);
                if (lb.pictureType == NV_ENC_PIC_TYPE_IDR || lb.pictureType == NV_ENC_PIC_TYPE_I) idr++;
                got++; total += lb.bitstreamSizeInBytes;
            }
            fl.nvEncUnlockBitstream(enc, bb.bitstreamBuffer);
        }
        printf("150 frames -> %d output, %d keyframes, %d total bytes\n", got, idr, total);
    }
    fl.nvEncDestroyEncoder(enc);
    printf("DONE\n");
    return 0;
}
