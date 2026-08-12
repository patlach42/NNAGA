#include "SharedRing.h"
#include "../util/log.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>

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
    data_->shared_feature_bits = VSTPOC_FEATURE_PLANAR_AUDIO |
                                 VSTPOC_FEATURE_WAKE_SOCKET |
                                 VSTPOC_FEATURE_MIDI_EVENTS;
    wakePath_ = path + ".wake";
    if (wakePath_.size() < sizeof(sockaddr_un::sun_path)) {
        wakeListener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (wakeListener_ >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, wakePath_.c_str(), sizeof(addr.sun_path) - 1);
            ::unlink(addr.sun_path);
            if (::bind(wakeListener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                ::listen(wakeListener_, 1) == 0) {
                wakeStop_.store(false, std::memory_order_release);
                wakeThread_ = std::thread(&SharedRing::acceptLoop, this);
            } else {
                ::close(wakeListener_); wakeListener_ = -1;
            }
        }
    }
    data_->shared_layout_size = static_cast<uint32_t>(sizeof(VstpocShared));
    LOGI("SharedRing: mapped %s (%zu bytes)", path.c_str(), sizeof(VstpocShared));
}
SharedRing::~SharedRing() {
    wakeStop_.store(true, std::memory_order_release);
    if (wakeListener_ >= 0) ::close(wakeListener_);
    if (wakeThread_.joinable()) wakeThread_.join();
    int c = wakeClient_.exchange(-1, std::memory_order_acq_rel);
    if (c >= 0) ::close(c);
    if (!wakePath_.empty()) ::unlink(wakePath_.c_str());
    if (data_) ::munmap(data_, sizeof(VstpocShared));
    if (fd_ >= 0) ::close(fd_);
}

void SharedRing::acceptLoop() {
    while (!wakeStop_.load(std::memory_order_acquire)) {
        int c = ::accept4(wakeListener_, nullptr, nullptr, SOCK_NONBLOCK);
        if (c >= 0) {
            int old = wakeClient_.exchange(c, std::memory_order_acq_rel);
            if (old >= 0) ::close(old);
            return;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void SharedRing::notifyWake() const {
    int c = wakeClient_.load(std::memory_order_acquire);
    if (c >= 0) {
        const uint8_t b = 1;
        if (::send(c, &b, 1, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 &&
            (errno == EPIPE || errno == ECONNRESET)) {
            int expected = c;
            if (wakeClient_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel))
                ::close(c);
        }
    }
}

int32_t SharedRing::pullAudio(float* outL, float* outR, int32_t maxFrames) {
    if (!data_ || !outL || !outR || maxFrames <= 0) return 0;

    const uint64_t head = __atomic_load_n(&data_->audio_head, __ATOMIC_ACQUIRE);
    const uint64_t tail = __atomic_load_n(&data_->audio_tail, __ATOMIC_RELAXED);
    const uint64_t available = head - tail;
    const uint64_t want = static_cast<uint64_t>(maxFrames);
    const uint64_t take = available < want ? available : want;
    if (take == 0) return 0;

    constexpr uint64_t mask = VSTPOC_AUDIO_RING_FRAMES - 1u;
    const uint64_t slot = tail & mask;
    const uint64_t first = std::min<uint64_t>(take, VSTPOC_AUDIO_RING_FRAMES - slot);
    std::memcpy(outL, &data_->audio[0][slot], first * sizeof(float));
    std::memcpy(outR, &data_->audio[1][slot], first * sizeof(float));
    const uint64_t second = take - first;
    if (second != 0) {
        std::memcpy(outL + first, data_->audio[0], second * sizeof(float));
        std::memcpy(outR + first, data_->audio[1], second * sizeof(float));
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
                                   uint32_t blockFrames,
                                   const guitarrackcraft::MidiEvent* midiEvents,
                                   uint32_t midiEventCount) {
    if (!data_ || blockFrames == 0 || blockFrames > VSTPOC_MAX_BLOCK_FRAMES) return false;
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
    const uint32_t count = (midiEvents && midiEventCount < VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK)
                             ? midiEventCount : (midiEvents ? VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK : 0u);
    b.midi_event_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& e = midiEvents[i];
        VstpocMidiEvent& out = b.midi_events[i];
        out.frame_offset = e.frameOffset < blockFrames ? e.frameOffset : blockFrames - 1u;
        out.status = e.status; out.data1 = e.data1; out.data2 = e.data2; out.reserved = 0;
    }
    __atomic_store_n(&data_->transport_queue_head, qh + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&data_->transport_seq, qh + 2u, __ATOMIC_RELEASE);
    if (qh == qt) notifyWake();
    return true;
}

uint32_t SharedRing::readMidiOutput(guitarrackcraft::MidiEvent* outputEvents,
                                    uint32_t outputCapacity) const {
    if (!data_ || !outputEvents || outputCapacity == 0) return 0;
    const uint64_t seq = __atomic_load_n(&data_->midi_output_seq, __ATOMIC_ACQUIRE);
    uint32_t count = data_->midi_output_count;
    if (count > VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK) count = VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK;
    if (count > outputCapacity) count = outputCapacity;
    for (uint32_t i = 0; i < count; ++i) {
        const VstpocMidiEvent& in = data_->midi_output_events[i];
        outputEvents[i].frameOffset = in.frame_offset;
        outputEvents[i].status = in.status;
        outputEvents[i].data1 = in.data1;
        outputEvents[i].data2 = in.data2;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&data_->midi_output_seq, __ATOMIC_ACQUIRE) != seq) return 0;
    return count;
}

int32_t SharedRing::pushInput(const float* left, const float* right, int32_t numFrames) {
    if (!data_ || !left || !right || numFrames <= 0) return 0;
    const uint64_t head = __atomic_load_n(&data_->audio_in_head, __ATOMIC_RELAXED);
    const uint64_t tail = __atomic_load_n(&data_->audio_in_tail, __ATOMIC_ACQUIRE);
    const uint64_t want = static_cast<uint64_t>(numFrames);
    if (head - tail + want > VSTPOC_AUDIO_RING_FRAMES) return 0;

    constexpr uint64_t mask = VSTPOC_AUDIO_RING_FRAMES - 1u;
    const uint64_t slot = head & mask;
    const uint64_t first = std::min<uint64_t>(want, VSTPOC_AUDIO_RING_FRAMES - slot);
    std::memcpy(&data_->audio_in[0][slot], left, first * sizeof(float));
    std::memcpy(&data_->audio_in[1][slot], right, first * sizeof(float));
    const uint64_t second = want - first;
    if (second != 0) {
        std::memcpy(data_->audio_in[0], left + first, second * sizeof(float));
        std::memcpy(data_->audio_in[1], right + first, second * sizeof(float));
    }
    __atomic_store_n(&data_->audio_in_head, head + want, __ATOMIC_RELEASE);
    if (head == tail) notifyWake();
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
    if (head - tail >= VSTPOC_PARAM_RING_MSGS) return;
    data_->params[head & (VSTPOC_PARAM_RING_MSGS - 1)] = {index, value};
    __atomic_store_n(&data_->param_head, head + 1, __ATOMIC_RELEASE);
    notifyWake();
}

void SharedRing::signalStop() {
    if (!data_) return;
    __atomic_store_n(&data_->stop_flag, 1, __ATOMIC_RELEASE);
    notifyWake();
}

void SharedRing::notifyGuest() { notifyWake(); }
bool SharedRing::guestReady() const {
    if (!data_) return false;
    return __atomic_load_n(&data_->guest_ready, __ATOMIC_ACQUIRE) != 0;
}

uint64_t SharedRing::guestFramesProduced() const {
    if (!data_) return 0;
    return __atomic_load_n(&data_->guest_frames_produced, __ATOMIC_RELAXED);
}
