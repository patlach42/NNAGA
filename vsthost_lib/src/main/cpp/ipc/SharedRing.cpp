#include "SharedRing.h"
#include "../util/log.h"
#include <algorithm>
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
    __atomic_store_n(&data_->shared_layout_magic, VSTPOC_SHARED_LAYOUT_MAGIC, __ATOMIC_RELAXED);
    __atomic_store_n(&data_->shared_layout_version, VSTPOC_SHARED_LAYOUT_VERSION, __ATOMIC_RELAXED);
    data_->shared_feature_bits = VSTPOC_FEATURE_PLANAR_AUDIO |
                                 VSTPOC_FEATURE_MIDI_EVENTS |
                                 VSTPOC_FEATURE_MIDI_OUTPUT;
    __atomic_store_n(&data_->shared_layout_size, static_cast<uint32_t>(VSTPOC_SHARED_LAYOUT_V8_SIZE),
                     __ATOMIC_RELEASE);
    LOGI("SharedRing: mapped %s (%zu bytes)", path.c_str(), sizeof(VstpocShared));
}
SharedRing::~SharedRing() {
    if (data_) ::munmap(data_, sizeof(VstpocShared));
    if (fd_ >= 0) ::close(fd_);
}


int32_t SharedRing::pullAudio(float* outL, float* outR, int32_t maxFrames) {
    if (!data_ || !outL || !outR || maxFrames <= 0) return 0;
    const uint64_t want = static_cast<uint64_t>(maxFrames);
    uint64_t tail = __atomic_load_n(&data_->output_block_tail, __ATOMIC_RELAXED);
    const uint64_t head = __atomic_load_n(&data_->output_block_head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        VstpocOutputBlock& block =
            data_->output_blocks[tail & (VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u)];
        const uint64_t sequence = __atomic_load_n(&block.sequence, __ATOMIC_ACQUIRE);
        if (sequence != tail + 1u) break; /* producer has not committed */
        const uint32_t frames = block.frame_count;
        const uint32_t offset = block.ring_offset;
        ++tail; /* consumer owns descriptor from this point */
        __atomic_store_n(&data_->output_block_tail, tail, __ATOMIC_RELEASE);
        const uint64_t audioTail =
            __atomic_load_n(&data_->audio_tail, __ATOMIC_RELAXED);
        if (frames == 0 || frames > VSTPOC_MAX_BLOCK_FRAMES ||
            offset >= VSTPOC_AUDIO_RING_FRAMES) {
            continue;
        }
        if (frames != want || tail != head) {
            __atomic_store_n(&data_->audio_tail, audioTail + frames, __ATOMIC_RELEASE);
            continue; /* trim stale/mismatched complete blocks, never partial */
        }
        const uint32_t first = std::min<uint32_t>(
            frames, VSTPOC_AUDIO_RING_FRAMES - offset);
        std::memcpy(outL, &data_->audio[0][offset], first * sizeof(float));
        std::memcpy(outR, &data_->audio[1][offset], first * sizeof(float));
        const uint32_t second = frames - first;
        if (second != 0) {
            std::memcpy(outL + first, data_->audio[0], second * sizeof(float));
            std::memcpy(outR + first, data_->audio[1], second * sizeof(float));
        }
        __atomic_store_n(&data_->audio_tail, audioTail + frames, __ATOMIC_RELEASE);
        return static_cast<int32_t>(frames);
    }
    return 0;
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
    const uint64_t deadlineBudgetNs = sampleRate > 0.0
        ? static_cast<uint64_t>(
              static_cast<double>(blockFrames) * 1'000'000'000.0 / sampleRate)
        : 0;
    __atomic_store_n(&data_->block_deadline_ns, deadlineBudgetNs, __ATOMIC_RELAXED);
    __atomic_store_n(&data_->transport_queue_head, qh + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&data_->transport_seq, qh + 2u, __ATOMIC_RELEASE);
    if (qh == qt) __atomic_store_n(&data_->wake_requested, 1u, __ATOMIC_RELEASE);
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
    if (head == tail) __atomic_store_n(&data_->wake_requested, 1u, __ATOMIC_RELEASE);
    return numFrames;
}

void SharedRing::setMicActive(bool active) {
    if (!data_) return;
    __atomic_store_n(&data_->mic_active, active ? 1 : 0, __ATOMIC_RELEASE);
}

void SharedRing::pushParam(int32_t index, float value) {
    if (!data_ || index < 0 || index >= static_cast<int32_t>(VSTPOC_MAX_PARAMS)) return;
    // v7 uses one lossless latest-value mailbox per parameter. Publish the
    // value before the release sequence increment so the guest control thread
    // observes a coherent value and can never lose the newest drag position.
    data_->param_desired_values[index] = value;
    __atomic_add_fetch(&data_->param_desired_seq[index], UINT64_C(1), __ATOMIC_RELEASE);
    __atomic_store_n(&data_->wake_requested, 1u, __ATOMIC_RELEASE);
}

void SharedRing::signalStop() {
    if (!data_) return;
    __atomic_store_n(&data_->stop_flag, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&data_->wake_requested, 1u, __ATOMIC_RELEASE);
}

void SharedRing::notifyGuest() {
    if (data_) __atomic_store_n(&data_->wake_requested, 1u, __ATOMIC_RELEASE);
}
bool SharedRing::guestReady() const {
    if (!data_) return false;
    return __atomic_load_n(&data_->guest_ready, __ATOMIC_ACQUIRE) != 0;
}

uint64_t SharedRing::guestFramesProduced() const {
    if (!data_) return 0;
    return __atomic_load_n(&data_->guest_frames_produced, __ATOMIC_RELAXED);
}
uint64_t SharedRing::starvationCount() const noexcept {
    return data_ ? __atomic_load_n(&data_->starvation_count, __ATOMIC_RELAXED) : 0;
}
uint64_t SharedRing::outputDropCount() const noexcept {
    return data_ ? __atomic_load_n(&data_->output_drop_count, __ATOMIC_RELAXED) : 0;
}
uint64_t SharedRing::guestDeadlineNs() const noexcept {
    return data_ ? __atomic_load_n(&data_->block_deadline_ns, __ATOMIC_RELAXED) : 0;
}
uint64_t SharedRing::deadlineMissCount() const noexcept {
    return data_ ? __atomic_load_n(&data_->deadline_miss_count, __ATOMIC_RELAXED) : 0;
}
