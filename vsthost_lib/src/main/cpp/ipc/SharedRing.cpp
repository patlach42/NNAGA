#include "SharedRing.h"
#include "../util/log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

SharedRing::SharedRing(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) {
        LOGE("SharedRing: open(%s) failed: %s", path.c_str(), std::strerror(errno));
        return;
    }
    if (::ftruncate(fd_, sizeof(VstpocShared)) != 0) {
        LOGE("SharedRing: ftruncate failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return;
    }
    void* p = ::mmap(nullptr, sizeof(VstpocShared),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        LOGE("SharedRing: mmap failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return;
    }
    data_ = static_cast<VstpocShared*>(p);
    std::memset(data_, 0, sizeof(VstpocShared));
    data_->shared_layout_magic = VSTPOC_SHARED_LAYOUT_MAGIC;
    data_->shared_layout_version = VSTPOC_SHARED_LAYOUT_VERSION;
    data_->shared_layout_size = static_cast<uint32_t>(sizeof(VstpocShared));
    LOGI("SharedRing: mapped %s (%zu bytes)", path.c_str(), sizeof(VstpocShared));
}

SharedRing::~SharedRing() {
    if (data_) ::munmap(data_, sizeof(VstpocShared));
    if (fd_ >= 0) ::close(fd_);
}

int32_t SharedRing::pullAudio(float* outL, float* outR, int32_t maxFrames) {
    if (!data_) return 0;

    const uint64_t head = __atomic_load_n(&data_->audio_head, __ATOMIC_ACQUIRE);
    const uint64_t tail = __atomic_load_n(&data_->audio_tail, __ATOMIC_RELAXED);
    const uint64_t available = head - tail;
    const uint64_t want = static_cast<uint64_t>(maxFrames);
    const uint64_t take = available < want ? available : want;

    for (uint64_t i = 0; i < take; ++i) {
        const uint64_t slot = (tail + i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
        outL[i] = data_->audio[slot * VSTPOC_CHANNELS + 0];
        outR[i] = data_->audio[slot * VSTPOC_CHANNELS + 1];
    }

    __atomic_store_n(&data_->audio_tail, tail + take, __ATOMIC_RELEASE);
    return static_cast<int32_t>(take);
}

bool SharedRing::inputWritable(uint32_t frames) const {
    if (!data_) return false;
    const uint64_t head = __atomic_load_n(&data_->audio_in_head, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&data_->audio_in_tail, __ATOMIC_ACQUIRE);
    return head - tail + frames <= VSTPOC_AUDIO_RING_FRAMES;
}

bool SharedRing::publishTransport(uint64_t samplePosition, uint64_t transportFrame,
                                   uint64_t loopEndFrame, double sampleRate,
                                   double beatsPerMinute, bool playing, bool looping,
                                   uint32_t blockFrames) {
    if (!data_) return false;
    uint64_t qh = __atomic_load_n(&data_->transport_queue_head, __ATOMIC_RELAXED);
    uint64_t qt = __atomic_load_n(&data_->transport_queue_tail, __ATOMIC_ACQUIRE);
    if (qh - qt >= VSTPOC_TRANSPORT_QUEUE_CAPACITY) {
        __atomic_add_fetch(&data_->transport_queue_dropped, 1u, __ATOMIC_RELAXED);
        return false;
    }
    VstpocTransportBlock& b = data_->transport_queue[qh & (VSTPOC_TRANSPORT_QUEUE_CAPACITY - 1u)];
    b.sample_position = samplePosition;
    b.transport_frame = transportFrame;
    b.loop_end_frame = loopEndFrame;
    b.sample_rate = sampleRate;
    b.beats_per_minute = beatsPerMinute;
    b.flags = (playing ? 1u : 0u) | (looping ? 2u : 0u);
    b.block_frames = blockFrames;
    __atomic_store_n(&data_->transport_queue_head, qh + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&data_->transport_seq, qh + 2u, __ATOMIC_RELEASE);
    return true;
}

int32_t SharedRing::pushInput(const float* interleavedStereo, int32_t numFrames) {
    if (!data_ || !interleavedStereo || numFrames < 0) return 0;
    const uint64_t head = __atomic_load_n(&data_->audio_in_head, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&data_->audio_in_tail, __ATOMIC_ACQUIRE);
    const uint64_t want = static_cast<uint64_t>(numFrames);
    if (head - tail + want > VSTPOC_AUDIO_RING_FRAMES) return 0;
    for (uint64_t i = 0; i < want; ++i) {
        const uint64_t slot = (head + i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
        data_->audio_in[slot * VSTPOC_CHANNELS + 0] = interleavedStereo[i * 2 + 0];
        data_->audio_in[slot * VSTPOC_CHANNELS + 1] = interleavedStereo[i * 2 + 1];
    }
    __atomic_store_n(&data_->audio_in_head, head + want, __ATOMIC_RELEASE);
    return numFrames;
}

void SharedRing::setMicActive(bool active) {
    if (!data_) return;
    __atomic_store_n(&data_->mic_active, active ? 1 : 0, __ATOMIC_RELEASE);
}

void SharedRing::pushParam(int32_t index, float value) {
    if (!data_) return;

    const uint64_t head = __atomic_load_n(&data_->param_head, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&data_->param_tail, __ATOMIC_ACQUIRE);
    if (head - tail >= VSTPOC_PARAM_RING_MSGS) {
        return;  // ring full; drop
    }
    data_->params[head & (VSTPOC_PARAM_RING_MSGS - 1)] = {index, value};
    __atomic_store_n(&data_->param_head, head + 1, __ATOMIC_RELEASE);
}

void SharedRing::signalStop() {
    if (!data_) return;
    __atomic_store_n(&data_->stop_flag, 1, __ATOMIC_RELEASE);
}

bool SharedRing::guestReady() const {
    if (!data_) return false;
    return __atomic_load_n(&data_->guest_ready, __ATOMIC_ACQUIRE) != 0;
}

uint64_t SharedRing::guestFramesProduced() const {
    if (!data_) return 0;
    return __atomic_load_n(&data_->guest_frames_produced, __ATOMIC_RELAXED);
}
