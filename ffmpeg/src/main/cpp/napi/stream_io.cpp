#include "stream_io.h"

#include <cstdint>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <hilog/log.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ffmpeg {
namespace {

constexpr int STREAM_IO_BUFFER_SIZE = 64 * 1024;
constexpr int STREAM_IO_TIMEOUT_MS = 15000;
constexpr size_t STREAM_IO_MAX_ENTRIES = 32;
constexpr char STREAM_IO_PREFIX[] = "streamio:";

// JS callback types dispatched through the tsfn.
enum StreamIOCallType {
    STREAM_IO_CALL_READ = 0,
    STREAM_IO_CALL_SEEK = 1,
    STREAM_IO_CALL_CLOSE = 2,
};

// Seek whence codes handed to JS (SEEK_CUR is resolved to absolute natively).
constexpr int64_t STREAM_IO_SEEK_SET = 0;
constexpr int64_t STREAM_IO_SEEK_SIZE = 2;
constexpr int64_t STREAM_IO_SEEK_END = 3;

// Per-request response slot. The demux thread registers a slot under the
// entry mutex before dispatching the callback; pushStreamIO (JS thread)
// fills it; the demux thread consumes it on wake.
struct StreamResponse {
    int64_t value = -1;
    std::vector<uint8_t> data;
    bool done = false;
};

struct StreamEntry {
    napi_ref readRef = nullptr;  // (seq, offset, length) => void
    napi_ref seekRef = nullptr;  // (seq, offset, whence) => void
    napi_ref closeRef = nullptr; // () => void
    napi_threadsafe_function tsfn = nullptr;
    bool seekable = false; // immutable after registration (thread-safe read)

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> closed{ false };
    std::atomic<int64_t> pos{ 0 };
    uint64_t seq = 0; // last dispatched request id (guarded by mtx)
    std::unordered_map<uint64_t, StreamResponse*> pending;
};

struct StreamIOHandle {
    std::shared_ptr<StreamEntry> entry;
};

struct TsfnContext {
    std::shared_ptr<StreamEntry> entry;
};

struct StreamIOMsg {
    std::shared_ptr<StreamEntry> entry;
    int32_t type = STREAM_IO_CALL_READ;
    uint64_t seq = 0;
    int64_t offset = 0;
    int64_t length = 0;
};

std::mutex g_streamIoMtx;
std::unordered_map<int64_t, std::shared_ptr<StreamEntry>> g_streamEntries;
std::unordered_set<AVIOContext*> g_liveAvios;

bool ParseStreamIOId(const std::string& url, int64_t& id)
{
    const size_t prefixLen = sizeof(STREAM_IO_PREFIX) - 1;
    if (url.size() <= prefixLen || url.compare(0, prefixLen, STREAM_IO_PREFIX) != 0) {
        return false;
    }
    int64_t value = 0;
    const size_t digits = url.size() - prefixLen;
    if (digits > 18) {
        return false;
    }
    for (size_t i = prefixLen; i < url.size(); ++i) {
        const char c = url[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value <= 0) {
        return false;
    }
    id = value;
    return true;
}

// Dispatch a callback invocation to the JS thread. Acquires a transient tsfn
// reference so a concurrent closeStreamIO cannot finalize the function while
// the call is in flight.
bool DispatchCall(const std::shared_ptr<StreamEntry>& entry, int32_t type, uint64_t seq,
    int64_t offset, int64_t length)
{
    if (entry->tsfn == nullptr || entry->closed.load()) {
        return false;
    }
    if (napi_acquire_threadsafe_function(entry->tsfn) != napi_ok) {
        return false;
    }
    auto* msg = new StreamIOMsg();
    msg->entry = entry;
    msg->type = type;
    msg->seq = seq;
    msg->offset = offset;
    msg->length = length;
    napi_status st = napi_call_threadsafe_function(entry->tsfn, msg, napi_tsfn_nonblocking);
    napi_release_threadsafe_function(entry->tsfn, napi_tsfn_release);
    if (st != napi_ok) {
        delete msg;
        return false;
    }
    return true;
}

// Wait for the JS response of request `seq`. `slot` is owned by the caller
// and stays registered in entry->pending until consumed, timed out or closed.
bool WaitResponse(const std::shared_ptr<StreamEntry>& entry, uint64_t seq,
    std::unique_ptr<StreamResponse>& slot, bool& timedOut)
{
    std::unique_lock<std::mutex> lock(entry->mtx);
    const bool ok = entry->cv.wait_for(lock, std::chrono::milliseconds(STREAM_IO_TIMEOUT_MS),
        [&entry, &slot] { return slot->done || entry->closed.load(); });
    entry->pending.erase(seq);
    if (!slot->done) {
        timedOut = !entry->closed.load();
        return false;
    }
    return true;
}

int StreamIORead(void* opaque, uint8_t* buf, int bufSize)
{
    auto* handle = static_cast<StreamIOHandle*>(opaque);
    const std::shared_ptr<StreamEntry> entry = handle->entry;
    if (entry->closed.load()) {
        return AVERROR_EOF;
    }
    uint64_t seq = 0;
    std::unique_ptr<StreamResponse> slot(new StreamResponse());
    {
        std::lock_guard<std::mutex> lock(entry->mtx);
        seq = ++entry->seq;
        entry->pending[seq] = slot.get();
    }
    if (!DispatchCall(entry, STREAM_IO_CALL_READ, seq, entry->pos.load(), bufSize)) {
        std::lock_guard<std::mutex> lock(entry->mtx);
        entry->pending.erase(seq);
        return AVERROR(EIO);
    }
    bool timedOut = false;
    if (!WaitResponse(entry, seq, slot, timedOut)) {
        if (timedOut) {
            // A stalled JS producer would otherwise stall every following
            // call for the full timeout; fail the source permanently.
            entry->closed.store(true);
            entry->cv.notify_all();
        }
        return AVERROR_EOF;
    }
    const int64_t value = slot->value;
    if (value < 0) {
        return AVERROR(EIO);
    }
    if (value == 0) {
        return AVERROR_EOF;
    }
    size_t n = slot->data.size();
    if (static_cast<int64_t>(n) > value) {
        n = static_cast<size_t>(value);
    }
    if (static_cast<int>(n) > bufSize) {
        n = static_cast<size_t>(bufSize);
    }
    if (n > 0) {
        memcpy(buf, slot->data.data(), n);
        entry->pos.fetch_add(static_cast<int64_t>(n));
    }
    return static_cast<int>(n);
}

int64_t StreamIOSeek(void* opaque, int64_t offset, int whence)
{
    auto* handle = static_cast<StreamIOHandle*>(opaque);
    const std::shared_ptr<StreamEntry> entry = handle->entry;
    if (entry->closed.load()) {
        return AVERROR_EOF;
    }
    if (whence == SEEK_CUR) {
        if (offset == 0) {
            return entry->pos.load();
        }
        offset += entry->pos.load();
        whence = SEEK_SET;
    }
    int64_t code = STREAM_IO_SEEK_SET;
    if (whence == AVSEEK_SIZE) {
        code = STREAM_IO_SEEK_SIZE;
    } else if (whence == SEEK_END) {
        code = STREAM_IO_SEEK_END;
    }
    uint64_t seq = 0;
    std::unique_ptr<StreamResponse> slot(new StreamResponse());
    {
        std::lock_guard<std::mutex> lock(entry->mtx);
        seq = ++entry->seq;
        entry->pending[seq] = slot.get();
    }
    if (!DispatchCall(entry, STREAM_IO_CALL_SEEK, seq, offset, code)) {
        std::lock_guard<std::mutex> lock(entry->mtx);
        entry->pending.erase(seq);
        return AVERROR(EIO);
    }
    bool timedOut = false;
    if (!WaitResponse(entry, seq, slot, timedOut)) {
        if (timedOut) {
            entry->closed.store(true);
            entry->cv.notify_all();
        }
        return AVERROR_EOF;
    }
    if (slot->value < 0) {
        return AVERROR(EIO);
    }
    if (whence != AVSEEK_SIZE) {
        entry->pos.store(slot->value);
    }
    return slot->value;
}

void StreamIODispatcher(napi_env env, napi_value jsCallback, void* context, void* data)
{
    (void)jsCallback;
    (void)context;
    auto* msg = static_cast<StreamIOMsg*>(data);
    std::shared_ptr<StreamEntry> entry = msg->entry;
    napi_value cb = nullptr;
    if (msg->type == STREAM_IO_CALL_READ && entry->readRef != nullptr) {
        if (napi_get_reference_value(env, entry->readRef, &cb) == napi_ok && cb != nullptr) {
            napi_value args[3] = { nullptr, nullptr, nullptr };
            napi_create_int64(env, static_cast<int64_t>(msg->seq), &args[0]);
            napi_create_int64(env, msg->offset, &args[1]);
            napi_create_int64(env, msg->length, &args[2]);
            napi_call_function(env, nullptr, cb, 3, args, nullptr);
        }
    } else if (msg->type == STREAM_IO_CALL_SEEK && entry->seekRef != nullptr) {
        if (napi_get_reference_value(env, entry->seekRef, &cb) == napi_ok && cb != nullptr) {
            napi_value args[3] = { nullptr, nullptr, nullptr };
            napi_create_int64(env, static_cast<int64_t>(msg->seq), &args[0]);
            napi_create_int64(env, msg->offset, &args[1]);
            napi_create_int64(env, msg->length, &args[2]); // whence code
            napi_call_function(env, nullptr, cb, 3, args, nullptr);
        }
    } else if (msg->type == STREAM_IO_CALL_CLOSE && entry->closeRef != nullptr) {
        if (napi_get_reference_value(env, entry->closeRef, &cb) == napi_ok && cb != nullptr) {
            napi_call_function(env, nullptr, cb, 0, nullptr, nullptr);
        }
    }
    delete msg;
}

void StreamIOFinalize(napi_env env, void* data, void* hint)
{
    (void)hint;
    auto* ctx = static_cast<TsfnContext*>(data);
    if (ctx != nullptr) {
        std::shared_ptr<StreamEntry> entry = ctx->entry;
        if (entry != nullptr) {
            if (entry->readRef != nullptr) {
                napi_delete_reference(env, entry->readRef);
                entry->readRef = nullptr;
            }
            if (entry->seekRef != nullptr) {
                napi_delete_reference(env, entry->seekRef);
                entry->seekRef = nullptr;
            }
            if (entry->closeRef != nullptr) {
                napi_delete_reference(env, entry->closeRef);
                entry->closeRef = nullptr;
            }
        }
        delete ctx;
    }
}

} // namespace

bool IsStreamIOUrl(const std::string& url)
{
    const size_t prefixLen = sizeof(STREAM_IO_PREFIX) - 1;
    return url.size() > prefixLen && url.compare(0, prefixLen, STREAM_IO_PREFIX) == 0;
}

bool AttachStreamIO(AVFormatContext* fmt, const std::string& url, std::string& err)
{
    int64_t id = 0;
    if (!ParseStreamIOId(url, id)) {
        return false;
    }
    std::shared_ptr<StreamEntry> entry;
    {
        std::lock_guard<std::mutex> lock(g_streamIoMtx);
        auto it = g_streamEntries.find(id);
        if (it == g_streamEntries.end()) {
            err = "streamio id " + std::to_string(id) + " is not registered";
            return false;
        }
        entry = it->second;
    }
    uint8_t* buffer = static_cast<uint8_t*>(av_malloc(STREAM_IO_BUFFER_SIZE));
    if (buffer == nullptr) {
        err = "streamio: out of memory";
        return false;
    }
    auto* handle = new StreamIOHandle();
    handle->entry = entry;
    AVIOContext* pb = avio_alloc_context(buffer, STREAM_IO_BUFFER_SIZE, 0, handle, StreamIORead,
        nullptr, entry->seekable ? StreamIOSeek : nullptr);
    if (pb == nullptr) {
        av_free(buffer);
        delete handle;
        err = "streamio: avio_alloc_context failed";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_streamIoMtx);
        g_liveAvios.insert(pb);
    }
    fmt->pb = pb;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "FFStreamIO",
        "attached streamio id=%{public}lld seekable=%{public}d",
        static_cast<long long>(id), entry->seekable ? 1 : 0);
    return true;
}

void ReleaseStreamIO(AVIOContext* pb)
{
    if (pb == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_streamIoMtx);
        if (g_liveAvios.erase(pb) == 0) {
            return; // not ours (libavformat-owned or already released)
        }
    }
    // The entry itself stays alive/usable: a demux session rebuild frees the
    // old AVIOContext and attaches a fresh one to the same source. Blocked
    // readers hold their own shared_ptr and never touch the AVIOContext, so
    // no wake-up is needed here.
    auto* handle = static_cast<StreamIOHandle*>(pb->opaque);
    av_free(pb->buffer);
    avio_context_free(&pb);
    delete handle;
}

StreamIOGuard::StreamIOGuard(AVFormatContext* fmt) : pb_(fmt != nullptr ? fmt->pb : nullptr) {}

StreamIOGuard::~StreamIOGuard()
{
    ReleaseStreamIO(pb_);
    pb_ = nullptr;
}

// ---- NAPI surface -------------------------------------------------------

napi_value FFRegisterStreamIO(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_type_error(env, nullptr, "registerStreamIO(id: number, callbacks: StreamIOCallbacks) requires id and callbacks");
        return nullptr;
    }
    int64_t id = 0;
    if (napi_get_value_int64(env, args[0], &id) != napi_ok || id <= 0) {
        napi_throw_type_error(env, nullptr, "id must be a positive number");
        return nullptr;
    }
    napi_valuetype objType = napi_undefined;
    napi_typeof(env, args[1], &objType);
    if (objType != napi_object) {
        napi_throw_type_error(env, nullptr, "callbacks must be { read(seq, offset, length), seek?(seq, offset, whence), close?() }");
        return nullptr;
    }
    auto entry = std::make_shared<StreamEntry>();
    napi_value readVal = nullptr;
    napi_value seekVal = nullptr;
    napi_value closeVal = nullptr;
    napi_valuetype t = napi_undefined;
    napi_get_named_property(env, args[1], "read", &readVal);
    napi_typeof(env, readVal, &t);
    if (t != napi_function) {
        napi_throw_type_error(env, nullptr, "callbacks.read must be a function(seq, offset, length)");
        return nullptr;
    }
    napi_create_reference(env, readVal, 1, &entry->readRef);
    napi_get_named_property(env, args[1], "seek", &seekVal);
    napi_typeof(env, seekVal, &t);
    if (t == napi_function) {
        napi_create_reference(env, seekVal, 1, &entry->seekRef);
        entry->seekable = true;
    }
    napi_get_named_property(env, args[1], "close", &closeVal);
    napi_typeof(env, closeVal, &t);
    if (t == napi_function) {
        napi_create_reference(env, closeVal, 1, &entry->closeRef);
    }
    {
        std::lock_guard<std::mutex> lock(g_streamIoMtx);
        if (g_streamEntries.size() >= STREAM_IO_MAX_ENTRIES) {
            napi_throw_error(env, nullptr, "too many registered streamio sources");
            return nullptr;
        }
        if (g_streamEntries.find(id) != g_streamEntries.end()) {
            napi_throw_error(env, nullptr, "streamio id is already registered");
            return nullptr;
        }
        g_streamEntries[id] = entry;
    }
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "ffmpeg_streamio_tsfn", NAPI_AUTO_LENGTH, &resourceName);
    auto* ctx = new TsfnContext();
    ctx->entry = entry;
    // max_queue_size 0 = unlimited: dispatching from the demux thread must
    // never block (a full queue would deadlock the demux <-> JS loop).
    napi_threadsafe_function tsfn = nullptr;
    napi_status st = napi_create_threadsafe_function(env, nullptr, nullptr, resourceName, 0, 1,
        ctx, StreamIOFinalize, nullptr, StreamIODispatcher, &tsfn);
    if (st != napi_ok) {
        delete ctx;
        std::lock_guard<std::mutex> lock(g_streamIoMtx);
        g_streamEntries.erase(id);
        napi_throw_error(env, nullptr, "failed to create streamio dispatcher");
        return nullptr;
    }
    entry->tsfn = tsfn;
    return nullptr;
}

napi_value FFPushStreamIO(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value args[4] = { nullptr, nullptr, nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    napi_value result = nullptr;
    napi_get_boolean(env, false, &result);
    if (argc < 3) {
        return result;
    }
    int64_t id = 0;
    int64_t seq64 = 0;
    int64_t value = 0;
    if (napi_get_value_int64(env, args[0], &id) != napi_ok ||
        napi_get_value_int64(env, args[1], &seq64) != napi_ok ||
        napi_get_value_int64(env, args[2], &value) != napi_ok) {
        return result;
    }
    const uint64_t seq = static_cast<uint64_t>(seq64);
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    if (argc >= 4 && args[3] != nullptr) {
        napi_valuetype t = napi_undefined;
        napi_typeof(env, args[3], &t);
        bool isArrayBuffer = false;
        if (t == napi_object && napi_is_arraybuffer(env, args[3], &isArrayBuffer) == napi_ok && isArrayBuffer) {
            void* ptr = nullptr;
            if (napi_get_arraybuffer_info(env, args[3], &ptr, &dataLen) == napi_ok) {
                data = static_cast<const uint8_t*>(ptr);
            }
        }
    }
    std::shared_ptr<StreamEntry> entry;
    {
        std::lock_guard<std::mutex> lock(g_streamIoMtx);
        auto it = g_streamEntries.find(id);
        if (it == g_streamEntries.end()) {
            return result;
        }
        entry = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(entry->mtx);
        auto pending = entry->pending.find(seq);
        if (pending == entry->pending.end()) {
            return result; // stale (timed out / closed / already answered)
        }
        StreamResponse* slot = pending->second;
        slot->value = value;
        if (data != nullptr && dataLen > 0 && value > 0) {
            size_t n = dataLen;
            if (static_cast<int64_t>(n) > value) {
                n = static_cast<size_t>(value);
            }
            slot->data.assign(data, data + n);
        }
        slot->done = true;
        entry->pending.erase(seq);
    }
    entry->cv.notify_all();
    napi_get_boolean(env, true, &result);
    return result;
}

napi_value FFCloseStreamIO(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_type_error(env, nullptr, "closeStreamIO(id: number) requires id");
        return nullptr;
    }
    int64_t id = 0;
    if (napi_get_value_int64(env, args[0], &id) != napi_ok || id <= 0) {
        napi_throw_type_error(env, nullptr, "id must be a positive number");
        return nullptr;
    }
    std::shared_ptr<StreamEntry> entry;
    {
        std::lock_guard<std::mutex> lock(g_streamIoMtx);
        auto it = g_streamEntries.find(id);
        if (it == g_streamEntries.end()) {
            return nullptr;
        }
        entry = it->second;
        g_streamEntries.erase(it);
    }
    entry->closed.store(true);
    entry->cv.notify_all();
    // Best-effort close notification, then drop the persistent tsfn
    // reference; in-flight transient references keep finalization away
    // until any running dispatch completes.
    DispatchCall(entry, STREAM_IO_CALL_CLOSE, 0, 0, 0);
    napi_release_threadsafe_function(entry->tsfn, napi_tsfn_release);
    entry->tsfn = nullptr;
    return nullptr;
}

} // namespace ffmpeg
