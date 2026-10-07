// Host-test stub for <libavcodec/packet.h>: the minimal AVPacket surface
// PacketQueue uses, with plain-C++ heap semantics. Only compiled into the
// host unit tests (FFMPEG_HOST_TEST); the real header is used on device.
#ifndef FFMPEG_HOST_STUB_PACKET_H
#define FFMPEG_HOST_STUB_PACKET_H

#include <cstdint>
#include <cstring>

#define AV_PKT_FLAG_KEY 0x0001
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))

typedef struct AVPacket {
    int stream_index = 0;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t duration = 0;
    int size = 0;
    int flags = 0;
    uint8_t* data = nullptr;
} AVPacket;

inline AVPacket* av_packet_alloc(void)
{
    return new AVPacket();
}

inline void av_packet_free(AVPacket** pkt)
{
    if (pkt == nullptr || *pkt == nullptr) {
        return;
    }
    delete *pkt;
    *pkt = nullptr;
}

inline int av_packet_ref(AVPacket* dst, const AVPacket* src)
{
    delete[] dst->data;
    dst->data = new uint8_t[src->size > 0 ? static_cast<size_t>(src->size) : 1];
    if (src->data != nullptr && src->size > 0) {
        std::memcpy(dst->data, src->data, static_cast<size_t>(src->size));
    }
    dst->size = src->size;
    dst->flags = src->flags;
    dst->pts = src->pts;
    dst->dts = src->dts;
    dst->duration = src->duration;
    dst->stream_index = src->stream_index;
    return 0;
}

inline void av_packet_move_ref(AVPacket* dst, AVPacket* src)
{
    delete[] dst->data;
    *dst = *src;
    src->data = nullptr;
    src->size = 0;
}

inline void av_packet_unref(AVPacket* pkt)
{
    delete[] pkt->data;
    pkt->data = nullptr;
    pkt->size = 0;
}

#endif
