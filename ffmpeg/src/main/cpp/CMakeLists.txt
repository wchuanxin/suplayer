cmake_minimum_required(VERSION 3.5.0)
project(ffmpeg_har)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
set(FFMPEG_PREBUILT ${NATIVERENDER_ROOT_PATH}/third_party/ffmpeg)

if(NOT DEFINED OHOS_ARCH)
    set(OHOS_ARCH arm64-v8a)
endif()

include_directories(
    ${FFMPEG_PREBUILT}/include
    ${NATIVERENDER_ROOT_PATH}
)

add_library(ffmpeg SHARED
    napi/ffmpeg_napi.cpp
    napi/probe.cpp
    napi/frame_extractor.cpp
    napi/audio_decoder.cpp
    napi/video_decoder.cpp
    napi/subtitle_decoder.cpp
    napi/hw_decoder.cpp
    napi/demux_session.cpp
    napi/packet_queue.cpp
    napi/hls_parse.cpp
    napi/hls_playlist.cpp
    napi/transcoder.cpp
)

target_link_libraries(ffmpeg PUBLIC
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libavformat.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libavfilter.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libswscale.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libswresample.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libavcodec.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libopenh264.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libdavs2.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libuavs3d.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libssl.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libcrypto.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libavutil.a
    ${FFMPEG_PREBUILT}/libs/${OHOS_ARCH}/libc++_shared.so
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_window.so
    libnative_buffer.so
    libnative_media_vdec.so
    libnative_media_codecbase.so
    libnative_media_core.so
    z
    m
    dl
)

target_compile_options(ffmpeg PRIVATE
    -ffunction-sections
    -fdata-sections
)

target_link_options(ffmpeg PRIVATE
    -Wl,--gc-sections
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libffmpeg.map
)
