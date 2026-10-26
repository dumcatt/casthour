cmake_minimum_required(VERSION 3.20)
project(nvenc_amd_shim CXX)
set(CMAKE_CXX_STANDARD 17)

set(BACKEND "AMF" CACHE STRING "Encoder backend: AMF | STUB")
set(AMF_DIR "${CMAKE_SOURCE_DIR}/third_party/AMF/amf" CACHE PATH "Path to AMF SDK amf/ dir")

set(SOURCES src/shim.cpp)
if(BACKEND STREQUAL "AMF")
    list(APPEND SOURCES
        src/amf_backend.cpp
        ${AMF_DIR}/public/common/AMFFactory.cpp
        ${AMF_DIR}/public/common/Thread.cpp
        ${AMF_DIR}/public/common/TraceAdapter.cpp
        ${AMF_DIR}/public/common/AMFSTL.cpp
        ${AMF_DIR}/public/common/CurrentTimeImpl.cpp
        ${AMF_DIR}/public/common/Windows/ThreadWindows.cpp)
endif()

add_library(nvEncodeAPI64 SHARED ${SOURCES} src/nvenc_shim.def)
set_target_properties(nvEncodeAPI64 PROPERTIES PREFIX "" OUTPUT_NAME "nvEncodeAPI64")
target_include_directories(nvEncodeAPI64 PRIVATE src)
target_compile_definitions(nvEncodeAPI64 PRIVATE BACKEND_${BACKEND})
target_link_libraries(nvEncodeAPI64 PRIVATE d3d9 ole32)

if(BACKEND STREQUAL "AMF")
    target_include_directories(nvEncodeAPI64 PRIVATE "${AMF_DIR}")
endif()
