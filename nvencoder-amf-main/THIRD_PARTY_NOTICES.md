# Third-party notices

This project is MIT-licensed (see [LICENSE](LICENSE)). It uses the following
third-party components.

## NVIDIA Video Codec SDK header — `src/nvEncodeAPI.h`

Redistributed in this repo. Copyright (c) 2010-2024 NVIDIA Corporation, under a
permissive MIT-style license (permits use, copy, modify, publish, distribute,
sublicense, and sell, provided the copyright and permission notice are retained).
The full notice is kept at the top of the file.

## AMD Advanced Media Framework (AMF) SDK

Not redistributed. `build.bat` fetches the headers and a few `public/common`
sources from https://github.com/GPUOpen-LibrariesAndSDKs/AMF at build time. The
AMF SDK is MIT-licensed; its terms apply to those files where they are used.
The AMF runtime (`amfrt64.dll`) ships with the AMD GPU driver and is loaded at
run time — it is not distributed here.
