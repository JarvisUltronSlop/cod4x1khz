// Native implementation of the cinematic calls used by the engine. The
// original Win32 build continues importing the supplied Bink runtime.
#if defined(KHZ_NATIVE_VIDEO)
#include <binklib/bink.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
thread_local std::array<char, 512> errorText{};
thread_local std::vector<U32> requestedTracks;
thread_local U32 requestedIO = 131072;
thread_local BINKMEMALLOC movieAlloc = nullptr;
thread_local BINKMEMFREE movieFree = nullptr;

void setError(const char* text) { std::snprintf(errorText.data(), errorText.size(), "%s", text); }
void check(int result, const char* operation) {
    if (result >= 0) return;
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, detail, sizeof(detail));
    throw std::runtime_error(std::string(operation) + ": " + detail);
}
struct Audio {
    int stream = -1;
    U32 id = 0;
    AVCodecContext* decoder = nullptr;
    SwrContext* resampler = nullptr;
    ALuint source = 0;
    std::vector<ALuint> buffers;
    float gain = 1.0f;
};
struct Movie : BINK {
    AVFormatContext* format = nullptr;
    AVIOContext* memoryIO = nullptr;
    const uint8_t* memory = nullptr;
    int64_t memoryBytes = 0, memoryAt = 0;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* audioFrame = nullptr;
    AVFrame* converted = nullptr;
    SwsContext* scaler = nullptr;
    int videoStream = -1;
    bool alpha = false, drained = false, decoded = false;
    Clock::time_point epoch{}, pausedAt{};
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;
    PFNALCSETTHREADCONTEXTPROC setThreadContext = nullptr;
    PFNALCGETTHREADCONTEXTPROC getThreadContext = nullptr;
    std::vector<Audio> audio;
    BINKMEMFREE release = nullptr;

    Movie() : BINK{} {}
    ~Movie();
};
Movie* movie(HBINK b) { return static_cast<Movie*>(b); }
struct AudioContext {
    Movie& owner;
    ALCcontext* previous = nullptr;
    explicit AudioContext(Movie& value) : owner(value) {
        if (owner.context) {
            previous = owner.getThreadContext();
            if (!owner.setThreadContext(owner.context)) throw std::runtime_error("Cinematic audio context activation failed");
        }
    }
    ~AudioContext() { if (owner.context) owner.setThreadContext(previous); }
};
void clearAudio(Audio& audio) {
    alSourceStop(audio.source);
    alSourcei(audio.source, AL_BUFFER, 0);
    if (!audio.buffers.empty()) alDeleteBuffers(static_cast<ALsizei>(audio.buffers.size()), audio.buffers.data());
    audio.buffers.clear();
}
Movie::~Movie() {
    if (context && setThreadContext && getThreadContext) {
        const auto previous = getThreadContext();
        setThreadContext(context);
        for (auto& track : audio) { clearAudio(track); alDeleteSources(1, &track.source); }
        setThreadContext(previous == context ? nullptr : previous);
    }
    for (auto& track : audio) { swr_free(&track.resampler); avcodec_free_context(&track.decoder); }
    if (context) alcDestroyContext(context);
    if (device) alcCloseDevice(device);
    av_frame_free(&converted);
    av_frame_free(&audioFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(scaler);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    if (memoryIO) { av_freep(&memoryIO->buffer); avio_context_free(&memoryIO); }
}
void dispose(Movie* value) {
    if (!value) return;
    const auto release = value->release;
    value->~Movie();
    if (release) release(value); else ::operator delete(value);
}
int readMemory(void* opaque, uint8_t* target, int bytes) {
    auto& value = *static_cast<Movie*>(opaque);
    bytes = static_cast<int>(std::min<int64_t>(bytes, value.memoryBytes - value.memoryAt));
    if (bytes <= 0) return AVERROR_EOF;
    std::memcpy(target, value.memory + value.memoryAt, bytes);
    value.memoryAt += bytes;
    return bytes;
}
int64_t seekMemory(void* opaque, int64_t offset, int whence) {
    auto& value = *static_cast<Movie*>(opaque);
    if (whence == AVSEEK_SIZE) return value.memoryBytes;
    whence &= ~AVSEEK_FORCE;
    int64_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = value.memoryAt;
    else if (whence == SEEK_END) base = value.memoryBytes;
    else return AVERROR(EINVAL);
    if (offset < -base || offset > value.memoryBytes - base) return AVERROR(EINVAL);
    value.memoryAt = base + offset;
    return value.memoryAt;
}
AVCodecContext* openDecoder(const AVCodecParameters* parameters) {
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) throw std::runtime_error("Cinematic codec is unavailable");
    auto* context = avcodec_alloc_context3(codec);
    if (!context) throw std::bad_alloc();
    try {
        check(avcodec_parameters_to_context(context, parameters), "Cinematic parameters");
        context->thread_count = 2;
        check(avcodec_open2(context, codec, nullptr), "Cinematic decoder");
        return context;
    } catch (...) { avcodec_free_context(&context); throw; }
}
void openAudio(Movie& value) {
    for (unsigned i = 0; i != value.format->nb_streams; ++i) {
        const auto* stream = value.format->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        ++value.NumTracks;
        const auto id = static_cast<U32>(stream->id);
        if (!requestedTracks.empty() && std::find(requestedTracks.begin(), requestedTracks.end(), id) == requestedTracks.end()) continue;
        Audio track{};
        track.stream = static_cast<int>(i); track.id = id;
        track.decoder = openDecoder(stream->codecpar);
        value.audio.push_back(track);
    }
    if (value.audio.empty()) return;
    value.device = alcOpenDevice(nullptr);
    if (!value.device) throw std::runtime_error("Cinematic audio device could not be opened");
    value.setThreadContext = reinterpret_cast<PFNALCSETTHREADCONTEXTPROC>(alcGetProcAddress(value.device, "alcSetThreadContext"));
    value.getThreadContext = reinterpret_cast<PFNALCGETTHREADCONTEXTPROC>(alcGetProcAddress(value.device, "alcGetThreadContext"));
    if (!value.setThreadContext || !value.getThreadContext) throw std::runtime_error("Cinematic audio requires OpenAL thread contexts");
    value.context = alcCreateContext(value.device, nullptr);
    if (!value.context) throw std::runtime_error("Cinematic audio context could not be created");
    AudioContext scope(value);
    for (auto& track : value.audio) {
        alGenSources(1, &track.source);
        alSourcei(track.source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(track.source, AL_ROLLOFF_FACTOR, 0.0f);
        if (alGetError() != AL_NO_ERROR) throw std::runtime_error("Cinematic audio source allocation failed");
    }
}
void queueAudio(Movie& value, Audio& track, AVFrame* frame) {
    if (!track.resampler) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(swr_alloc_set_opts2(&track.resampler, &stereo, AV_SAMPLE_FMT_S16, frame->sample_rate,
            &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr), "Cinematic audio resampler");
        check(swr_init(track.resampler), "Cinematic audio conversion");
    }
    const int capacity = swr_get_out_samples(track.resampler, frame->nb_samples);
    check(capacity, "Cinematic audio sample count");
    std::vector<int16_t> pcm(static_cast<size_t>(capacity) * 2);
    uint8_t* out = reinterpret_cast<uint8_t*>(pcm.data());
    const int samples = swr_convert(track.resampler, &out, capacity, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    check(samples, "Cinematic audio samples");
    if (!samples) return;
    AudioContext scope(value);
    ALint processed = 0;
    alGetSourcei(track.source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint old = 0; alSourceUnqueueBuffers(track.source, 1, &old);
        const auto found = std::find(track.buffers.begin(), track.buffers.end(), old);
        if (found != track.buffers.end()) track.buffers.erase(found);
        alDeleteBuffers(1, &old);
    }
    // Each video step decodes only its associated audio packets. Bound the
    // queue explicitly so malformed timing cannot consume unlimited memory.
    if (track.buffers.size() >= 256) throw std::runtime_error("Cinematic audio queue exceeded its limit");
    ALuint buffer = 0; alGenBuffers(1, &buffer);
    track.buffers.push_back(buffer);
    alBufferData(buffer, AL_FORMAT_STEREO16, pcm.data(), samples * 4, frame->sample_rate);
    alSourceQueueBuffers(track.source, 1, &buffer);
    ALint state = 0; alGetSourcei(track.source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING && !value.Paused) alSourcePlay(track.source);
    if (alGetError() != AL_NO_ERROR) throw std::runtime_error("Cinematic audio buffer submission failed");
}
void decodeAudio(Movie& value, Audio& track, const AVPacket* packet) {
    check(avcodec_send_packet(track.decoder, packet), "Cinematic audio packet");
    for (;;) {
        const int status = avcodec_receive_frame(track.decoder, value.audioFrame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
        check(status, "Cinematic audio decode");
        queueAudio(value, track, value.audioFrame);
        av_frame_unref(value.audioFrame);
    }
}
void decodeVideo(Movie& value) {
    for (;;) {
        const int status = avcodec_receive_frame(value.decoder, value.frame);
        if (status == 0) return;
        if (status != AVERROR(EAGAIN)) { check(status, "Cinematic video decode"); return; }
        const int read = av_read_frame(value.format, value.packet);
        if (read == AVERROR_EOF) {
            if (value.drained) throw std::runtime_error("Cinematic ended before its declared frame count");
            value.drained = true;
            check(avcodec_send_packet(value.decoder, nullptr), "Cinematic drain");
            for (auto& track : value.audio) decodeAudio(value, track, nullptr);
            continue;
        }
        check(read, "Cinematic packet read");
        try {
            if (value.packet->stream_index == value.videoStream)
                check(avcodec_send_packet(value.decoder, value.packet), "Cinematic video packet");
            else for (auto& track : value.audio)
                if (value.packet->stream_index == track.stream) decodeAudio(value, track, value.packet);
        } catch (...) { av_packet_unref(value.packet); throw; }
        av_packet_unref(value.packet);
    }
}
void copyPlane(const BINKPLANE& dest, const uint8_t* source, int stride, unsigned width, unsigned height) {
    if (!dest.Allocate) return;
    if (!dest.Buffer || !source || dest.BufferPitch < width) throw std::runtime_error("Invalid cinematic frame plane");
    auto* target = static_cast<uint8_t*>(dest.Buffer);
    for (unsigned y = 0; y != height; ++y) std::memcpy(target + size_t(y) * dest.BufferPitch, source + ptrdiff_t(y) * stride, width);
}
void copyFrame(Movie& value) {
    if (!value.FrameBuffers) throw std::runtime_error("Cinematic frame buffers were not registered");
    AVFrame* frame = value.frame;
    const auto targetFormat = value.alpha ? AV_PIX_FMT_YUVA420P : AV_PIX_FMT_YUV420P;
    if (frame->format != targetFormat) {
        value.scaler = sws_getCachedContext(value.scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, targetFormat, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!value.scaler) throw std::runtime_error("Cinematic plane conversion failed");
        if (!value.converted->data[0]) {
            value.converted->width = frame->width; value.converted->height = frame->height; value.converted->format = targetFormat;
            check(av_frame_get_buffer(value.converted, 32), "Cinematic conversion buffer");
        }
        check(av_frame_make_writable(value.converted), "Cinematic writable planes");
        check(sws_scale(value.scaler, frame->data, frame->linesize, 0, frame->height, value.converted->data, value.converted->linesize), "Cinematic plane scaling");
        frame = value.converted;
    }
    auto& set = *value.FrameBuffers;
    set.FrameNum = (value.FrameNum - 1) % 2;
    auto& planes = set.Frames[set.FrameNum];
    copyPlane(planes.YPlane, frame->data[0], frame->linesize[0], value.Width, value.Height);
    copyPlane(planes.cBPlane, frame->data[1], frame->linesize[1], (value.Width + 1) / 2, (value.Height + 1) / 2);
    copyPlane(planes.cRPlane, frame->data[2], frame->linesize[2], (value.Width + 1) / 2, (value.Height + 1) / 2);
    if (value.alpha) copyPlane(planes.APlane, frame->data[3], frame->linesize[3], value.Width, value.Height);
}
}

HBINK RADEXPLINK BinkOpen(const char* name, U32 flags) {
    setError("");
    Movie* value = nullptr;
    try {
        void* storage = movieAlloc ? movieAlloc(sizeof(Movie)) : ::operator new(sizeof(Movie));
        if (!storage) throw std::bad_alloc();
        value = new (storage) Movie;
        value->release = movieAlloc ? movieFree : nullptr;
        value->OpenFlags = flags;
        if (flags & BINKFROMMEMORY) {
            BINKHDR header{}; std::memcpy(&header, name, sizeof(header));
            const uint64_t bytes = uint64_t(header.Size) + 8;
            if (bytes < sizeof(header) || bytes > 512 * 1024 * 1024) throw std::runtime_error("Invalid memory-resident cinematic size");
            value->memory = reinterpret_cast<const uint8_t*>(name); value->memoryBytes = bytes;
            value->format = avformat_alloc_context();
            auto* buffer = static_cast<uint8_t*>(av_malloc(65536));
            if (!value->format || !buffer) { av_free(buffer); throw std::bad_alloc(); }
            value->memoryIO = avio_alloc_context(buffer, 65536, 0, value, readMemory, nullptr, seekMemory);
            if (!value->memoryIO) { av_free(buffer); throw std::bad_alloc(); }
            value->format->pb = value->memoryIO; value->format->flags |= AVFMT_FLAG_CUSTOM_IO;
            check(avformat_open_input(&value->format, nullptr, nullptr, nullptr), "Memory cinematic open");
        } else check(avformat_open_input(&value->format, name, nullptr, nullptr), "Cinematic open");
        check(avformat_find_stream_info(value->format, nullptr), "Cinematic stream information");
        value->videoStream = av_find_best_stream(value->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(value->videoStream, "Cinematic video stream");
        auto* stream = value->format->streams[value->videoStream];
        if (stream->codecpar->codec_id != AV_CODEC_ID_BINKVIDEO) throw std::runtime_error("Cinematic is not a Bink video");
        value->decoder = openDecoder(stream->codecpar);
        value->Width = value->decoder->width; value->Height = value->decoder->height;
        if (!value->Width || !value->Height || value->Width > 8192 || value->Height > 8192) throw std::runtime_error("Invalid cinematic dimensions");
        const auto rate = av_guess_frame_rate(value->format, stream, nullptr);
        if (rate.num <= 0 || rate.den <= 0) throw std::runtime_error("Invalid cinematic frame rate");
        value->FrameRate = rate.num; value->FrameRateDiv = rate.den;
        const int64_t frames = stream->nb_frames ? stream->nb_frames : av_rescale_q(stream->duration, stream->time_base, av_inv_q(rate));
        if (frames <= 0 || frames > std::numeric_limits<U32>::max()) throw std::runtime_error("Invalid cinematic frame count");
        value->Frames = static_cast<U32>(frames); value->FrameNum = 1;
        const auto* pixel = av_pix_fmt_desc_get(value->decoder->pix_fmt);
        value->alpha = (flags & BINKALPHA) && pixel && (pixel->flags & AV_PIX_FMT_FLAG_ALPHA);
        value->packet = av_packet_alloc(); value->frame = av_frame_alloc();
        value->audioFrame = av_frame_alloc(); value->converted = av_frame_alloc();
        if (!value->packet || !value->frame || !value->audioFrame || !value->converted) throw std::bad_alloc();
        openAudio(*value);
        return value;
    } catch (const std::exception& error) { setError(error.what()); dispose(value); return nullptr; }
}
void RADEXPLINK BinkClose(HBINK b) { dispose(movie(b)); }
char* RADEXPLINK BinkGetError() { return errorText.data(); }
void RADEXPLINK BinkSetMemory(BINKMEMALLOC alloc, BINKMEMFREE release) { movieAlloc = alloc; movieFree = release; }
void RADEXPLINK BinkSetSoundTrack(U32 count, U32* tracks) {
    requestedTracks.clear();
    if (count && tracks) requestedTracks.assign(tracks, tracks + std::min<U32>(count, 32));
}
void RADEXPLINK BinkSetIOSize(U32 size) { requestedIO = std::max<U32>(size, 1); }
void RADEXPLINK BinkGetFrameBuffersInfo(HBINK b, BINKFRAMEBUFFERS* buffers) {
    if (!b || !buffers) return;
    *buffers = {};
    buffers->TotalFrames = 2;
    buffers->YABufferWidth = b->Width; buffers->YABufferHeight = b->Height;
    buffers->cRcBBufferWidth = (b->Width + 1) / 2; buffers->cRcBBufferHeight = (b->Height + 1) / 2;
    for (auto& frame : buffers->Frames) {
        frame.YPlane.Allocate = frame.cRPlane.Allocate = frame.cBPlane.Allocate = 1;
        frame.APlane.Allocate = movie(b)->alpha;
    }
}
void RADEXPLINK BinkRegisterFrameBuffers(HBINK b, BINKFRAMEBUFFERS* buffers) { if (b) b->FrameBuffers = buffers; }
S32 RADEXPLINK BinkDoFrame(HBINK b) {
    if (!b) return 1;
    try {
        auto& value = *movie(b);
        if (!value.decoded) { decodeVideo(value); value.decoded = true; }
        copyFrame(value); b->LastFrameNum = b->FrameNum;
        return 0;
    } catch (const std::exception& error) { b->ReadError = 1; setError(error.what()); return 1; }
}
void RADEXPLINK BinkNextFrame(HBINK b) {
    if (!b) return;
    try {
        auto& value = *movie(b);
        av_frame_unref(value.frame); value.decoded = false;
        if (b->FrameNum < b->Frames) { ++b->FrameNum; return; }
        check(av_seek_frame(value.format, value.videoStream, 0, AVSEEK_FLAG_BACKWARD), "Cinematic loop seek");
        avcodec_flush_buffers(value.decoder);
        if (value.context) {
            AudioContext scope(value);
            for (auto& track : value.audio) { clearAudio(track); avcodec_flush_buffers(track.decoder); swr_free(&track.resampler); }
        }
        value.drained = false; b->FrameNum = 1; value.epoch = Clock::now();
    } catch (const std::exception& error) { b->ReadError = 1; setError(error.what()); }
}
S32 RADEXPLINK BinkWait(HBINK b) {
    if (!b || b->Paused) return 1;
    auto& value = *movie(b);
    if (value.epoch == Clock::time_point{}) value.epoch = Clock::now();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - value.epoch).count();
    return elapsed < double(b->FrameNum - 1) * b->FrameRateDiv / b->FrameRate;
}
S32 RADEXPLINK BinkPause(HBINK b, S32 pause) {
    if (!b) return 0;
    const S32 previous = b->Paused;
    if (bool(previous) == bool(pause)) return previous;
    auto& value = *movie(b);
    b->Paused = pause != 0;
    if (pause) value.pausedAt = Clock::now();
    else if (value.epoch != Clock::time_point{}) value.epoch += Clock::now() - value.pausedAt;
    if (value.context) {
        AudioContext scope(value);
        for (auto& track : value.audio) {
            if (pause) alSourcePause(track.source);
            else { ALint queued = 0; alGetSourcei(track.source, AL_BUFFERS_QUEUED, &queued); if (queued) alSourcePlay(track.source); }
        }
    }
    return previous;
}
void RADEXPLINK BinkGetRealtime(HBINK b, BINKREALTIME* realtime, U32) {
    if (!b || !realtime) return;
    *realtime = {}; realtime->FrameNum = b->FrameNum;
    realtime->FrameRate = b->FrameRate; realtime->FrameRateDiv = b->FrameRateDiv;
    // libavformat performs buffered reads on the cinematic thread; it has no
    // background prefetcher to suspend. Keep the engine's database handoff in
    // its completed-IO state between decode operations.
    realtime->ReadBufferSize = realtime->ReadBufferUsed = requestedIO;
}
S32 RADEXPLINK BinkControlBackgroundIO(HBINK b, U32 control) {
    if (!b) return 0;
    b->bio.Suspended = (control & ~BINKBGIOWAIT) == BINKBGIOSUSPEND;
    return b->bio.Suspended;
}
S32 RADEXPLINK BinkGetRects(HBINK b, U32) {
    if (!b) return 0;
    b->FrameRects[0] = {0, 0, static_cast<S32>(b->Width), static_cast<S32>(b->Height)};
    b->NumRects = 1; return 1;
}
void RADEXPLINK BinkSetMixBinVolumes(HBINK b, U32 id, U32*, S32* volumes, U32 count) {
    if (!b || !volumes || !count) return;
    auto& value = *movie(b);
    if (!value.context) return;
    AudioContext scope(value);
    for (auto& track : value.audio) if (track.id == id) {
        const auto volume = *std::max_element(volumes, volumes + std::min<U32>(count, 8));
        track.gain = std::clamp(volume / 32768.0f, 0.0f, 1.0f);
        alSourcef(track.source, AL_GAIN, track.gain);
    }
}
#endif
