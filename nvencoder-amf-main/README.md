# nvencoder-amf

Play recording is hard-wired to NVIDIA NVENC. This is a drop-in replacement `nvEncodeAPI64.dll` that implements the slice of the NVENC ABI the game uses and routes the encode to an AMD GPU via AMF.

## How it works

- [src/shim.cpp](src/shim.cpp) implements the NVENC function list the game calls — open/init/register/map/encode/lock, sequence params, async completion events, the legacy `nvEncOpenEncodeSession` null-check — and forwards frames to a backend.
- [src/amf_backend.cpp](src/amf_backend.cpp) is the AMD backend: it reads each D3D9 surface back to system memory, uploads to an AMF host surface, and encodes on a DX11-backed VCE encoder (CONSTQP with the game's QP, High profile, 1-in/1-out low latency, periodic keyframes for seeking).

Three constraints found along the way, all handled: the game null-checks the legacy `nvEncOpenEncodeSession` pointer; it parses the first frame's in-band SPS to build its MP4 media type (so the first `EncodePicture` must return real bytes — hence low-latency 1-in/1-out); and it needs a valid SPS/PPS from `nvEncGetSequenceParams`.

## Build

Requires the Visual Studio C++ build tools. `build.bat` auto-fetches the AMF SDK headers into `third_party/` on first run.

```
build.bat            AMD AMF backend (release)  -> dist\nvEncodeAPI64.dll
build.bat stub       stub backend (ABI debugging, emits no video)
```

Or with CMake: `cmake -B build -A x64 -DBACKEND=AMF .` then `cmake --build build --config Release`.

## Install

Copy `dist\nvEncodeAPI64.dll` into the game's `modules\` folder. Needs an AMD GPU with updated driver (provides `amfrt64.dll`).

## License

MIT — see [LICENSE](LICENSE). Bundles NVIDIA's `nvEncodeAPI.h` (MIT-style, notice retained) and builds against the AMD AMF SDK (MIT, fetched at build time). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).