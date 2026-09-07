#include "SharedRing.h"
#include "../util/log.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
SharedRing::SharedRing(const std::string& path, int reservedFd) {
    wake_path_ = path + ".wake";
    if (wake_path_.size() >= sizeof(((sockaddr_un*)nullptr)->sun_path)) {
        LOGE("SharedRing: wake path too long (%zu)", wake_path_.size());
    } else {
        wake_listener_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake_listener_fd_ >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, wake_path_.c_str(), wake_path_.size() + 1);
            ::unlink(wake_path_.c_str());
            if (::bind(wake_listener_fd_, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) != 0 || ::listen(wake_listener_fd_, 4) != 0) {
                LOGE("SharedRing: wake endpoint setup failed: %s", std::strerror(errno));
                ::close(wake_listener_fd_);
                wake_listener_fd_ = -1;
                ::unlink(wake_path_.c_str());
            } else {
                wake_running_.store(true, std::memory_order_release);
                wake_thread_ = std::thread(&SharedRing::wakeAcceptLoop, this);
            }
        }
    }
    fd_ = reservedFd >= 0
        ? reservedFd
        : ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
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
                                 VSTPOC_FEATURE_MIDI_OUTPUT |
                                 VSTPOC_FEATURE_MIDI_PAYLOAD_RING |
                                 (wakeReady() ? VSTPOC_FEATURE_WAKE_SOCKET : 0);
    __atomic_store_n(&data_->shared_layout_size, static_cast<uint32_t>(VSTPOC_SHARED_LAYOUT_V10_SIZE),
                     __ATOMIC_RELEASE);
    LOGI("SharedRing: mapped %s (%zu bytes)", path.c_str(), sizeof(VstpocShared));
}
void SharedRing::wakeAcceptLoop() {
    while (wake_running_.load(std::memory_order_acquire)) {
        int fd = ::accept4(wake_listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (!wake_running_.load(std::memory_order_acquire)) break;
            if (errno == EINTR) continue;
            usleep(1000);
            continue;
        }
        struct ucred cred{};
        socklen_t len = sizeof(cred);
        const pid_t expected = expected_wake_peer_.load(std::memory_order_acquire);
        if (expected <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
            cred.pid != expected) {
            ::close(fd);
            continue;
        }
        int expectedFd = -1;
        if (!wake_connection_fd_.compare_exchange_strong(
                expectedFd, fd, std::memory_order_acq_rel)) {
            ::close(fd);
        }
    }
}
void SharedRing::signalWake() noexcept {
    const int fd = wake_connection_fd_.load(std::memory_order_acquire);
    if (fd < 0) return;
    const unsigned char byte = 1;
    const ssize_t sent = ::send(fd, &byte, sizeof(byte), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)) {
        int expected = fd;
        if (wake_connection_fd_.compare_exchange_strong(expected, -1,
                                                         std::memory_order_acq_rel)) {
            ::close(fd);
        }
    }
}
SharedRing::~SharedRing() {
    wake_running_.store(false, std::memory_order_release);
    if (wake_listener_fd_ >= 0) {
        ::shutdown(wake_listener_fd_, SHUT_RDWR);
        ::close(wake_listener_fd_);
        wake_listener_fd_ = -1;
    }
    if (wake_thread_.joinable()) wake_thread_.join();
    int wakeFd = wake_connection_fd_.exchange(-1, std::memory_order_acq_rel);
    if (wakeFd >= 0) ::close(wakeFd);
    if (!wake_path_.empty()) ::unlink(wake_path_.c_str());
    if (data_) ::munmap(data_, sizeof(VstpocShared));
    if (fd_ >= 0) ::close(fd_);
}

int32_t SharedRing::pullAudio(float* outL, float* outR, int32_t maxFrames) {
    guitarrackcraft::MidiBuffer ignored;
    bool authoritative = false;
    return pullOutput(outL, outR, maxFrames, ignored, false, authoritative);
}

namespace {
static void copyRingOut(const uint8_t* ring, uint64_t absolute, uint32_t size,
                        uint8_t* dst) {
    const uint32_t slot = static_cast<uint32_t>(absolute & (VSTPOC_MIDI_PAYLOAD_RING_BYTES - 1u));
    const uint32_t first = std::min<uint32_t>(size, VSTPOC_MIDI_PAYLOAD_RING_BYTES - slot);
    std::memcpy(dst, ring + slot, first);
    if (size > first) std::memcpy(dst + first, ring, size - first);
}
static void copyRingIn(uint8_t* ring, uint64_t absolute, const uint8_t* src,
                       uint32_t size) {
    const uint32_t slot = static_cast<uint32_t>(absolute & (VSTPOC_MIDI_PAYLOAD_RING_BYTES - 1u));
    const uint32_t first = std::min<uint32_t>(size, VSTPOC_MIDI_PAYLOAD_RING_BYTES - slot);
    std::memcpy(ring + slot, src, first);
    if (size > first) std::memcpy(ring, src + first, size - first);
}
}

int32_t SharedRing::pullOutput(float* outL, float* outR, int32_t maxFrames,
                               guitarrackcraft::MidiBuffer& midi, bool oldest,
                               bool& authoritative) {
    midi.clear(); authoritative = false;
    if (!data_ || !outL || !outR || maxFrames <= 0) return 0;
    const uint64_t want = static_cast<uint64_t>(maxFrames);
    uint64_t tail = __atomic_load_n(&data_->output_block_tail, __ATOMIC_RELAXED);
    const uint64_t head = __atomic_load_n(&data_->output_block_head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        VstpocOutputBlock& block = data_->output_blocks[tail & (VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u)];
        if (__atomic_load_n(&block.sequence, __ATOMIC_ACQUIRE) != tail + 1u) {
            // The producer publishes the descriptor sequence before advancing
            // head. A mismatch is therefore an uncommitted block, not a stale
            // block to discard; advancing tail here permanently loses valid
            // guest output under the normal publication race.
            break;
        }
        ++tail;
        const uint32_t frames = block.frame_count;
        const uint32_t offset = block.ring_offset;
        const uint32_t count = block.midi_event_count;
        const uint64_t payloadBegin = block.midi_payload_begin;
        const uint64_t payloadEnd = block.midi_payload_end;
        const uint64_t payloadHead =
            __atomic_load_n(&data_->midi_output_payload_head, __ATOMIC_ACQUIRE);
        const uint64_t oldAudioTail =
            __atomic_load_n(&data_->audio_tail, __ATOMIC_RELAXED);
        const uint64_t audioHead =
            __atomic_load_n(&data_->audio_head, __ATOMIC_ACQUIRE);
        const uint64_t oldPayloadTail =
            __atomic_load_n(&data_->midi_output_payload_tail, __ATOMIC_RELAXED);
        const bool audioRangeValid =
            frames > 0 && frames <= VSTPOC_MAX_BLOCK_FRAMES &&
            offset == (oldAudioTail & (VSTPOC_AUDIO_RING_FRAMES - 1u)) &&
            frames <= audioHead - oldAudioTail;
        const bool payloadRangeValid =
            payloadBegin == oldPayloadTail && payloadEnd >= payloadBegin &&
            payloadEnd <= payloadHead &&
            payloadEnd - payloadBegin <= VSTPOC_MIDI_PAYLOAD_RING_BYTES;
        const bool metadataValid =
            count <= VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK;
        const bool valid = audioRangeValid && payloadRangeValid && metadataValid;
        const bool match = valid && frames == want && (oldest || tail == head);
        if (!valid || !match) {
            __atomic_store_n(
                &data_->audio_tail,
                audioRangeValid ? oldAudioTail + frames : audioHead,
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &data_->midi_output_payload_tail,
                payloadRangeValid ? payloadEnd : payloadHead,
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &data_->output_block_tail, tail, __ATOMIC_RELEASE);
            continue;
        }
        const uint32_t first = std::min<uint32_t>(frames, VSTPOC_AUDIO_RING_FRAMES - offset);
        std::memcpy(outL, &data_->audio[0][offset], first * sizeof(float));
        std::memcpy(outR, &data_->audio[1][offset], first * sizeof(float));
        if (frames > first) {
            std::memcpy(outL + first, data_->audio[0], (frames - first) * sizeof(float));
            std::memcpy(outR + first, data_->audio[1], (frames - first) * sizeof(float));
        }
        auto& payload = payload_scratch_;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& e = block.midi_events[i];
            if (e.payload_size == 0 || e.payload_size > 65536u ||
                e.payload_offset < payloadBegin || e.payload_offset > payloadEnd ||
                e.payload_size > payloadEnd - e.payload_offset ||
                e.frame_offset >= frames) {
                midi.recordRejectedMessages();
                continue;
            }
            copyRingOut(data_->midi_output_payload, e.payload_offset, e.payload_size, payload.data());
            (void)midi.append(e.frame_offset, payload.data(), e.payload_size);
        }
        authoritative = (block.midi_flags & VSTPOC_MIDI_FLAG_AUTHORITATIVE) != 0;
        __atomic_store_n(&data_->audio_tail, oldAudioTail + frames, __ATOMIC_RELEASE);
        __atomic_store_n(&data_->midi_output_payload_tail, payloadEnd, __ATOMIC_RELEASE);
        __atomic_store_n(&data_->output_block_tail, tail, __ATOMIC_RELEASE);
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
                                   const guitarrackcraft::MidiBuffer& midiEvents) {
    if (!data_ || blockFrames == 0 || blockFrames > VSTPOC_MAX_BLOCK_FRAMES) return false;
    const uint64_t qh = __atomic_load_n(&data_->transport_queue_head, __ATOMIC_RELAXED);
    const uint64_t qt = __atomic_load_n(&data_->transport_queue_tail, __ATOMIC_ACQUIRE);
    if (qh - qt >= VSTPOC_TRANSPORT_QUEUE_CAPACITY) {
        __atomic_add_fetch(&data_->transport_queue_dropped, 1u, __ATOMIC_RELAXED);
        return false;
    }
    VstpocTransportBlock& b = data_->transport_queue[qh & (VSTPOC_TRANSPORT_QUEUE_CAPACITY - 1u)];
    b.sample_position = samplePosition; b.transport_frame = transportFrame;
    b.loop_end_frame = loopEndFrame; b.sample_rate = sampleRate;
    b.beats_per_minute = beatsPerMinute;
    b.flags = (playing ? 1u : 0u) | (looping ? 2u : 0u);
    b.block_frames = blockFrames; b.midi_event_count = 0;
    uint64_t payloadHead = __atomic_load_n(&data_->midi_input_payload_head, __ATOMIC_RELAXED);
    const uint64_t payloadTail = __atomic_load_n(&data_->midi_input_payload_tail, __ATOMIC_ACQUIRE);
    const uint64_t payloadBegin = payloadHead;
    for (uint32_t i = 0; i < midiEvents.eventCount() &&
                         b.midi_event_count < VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK; ++i) {
        const auto& e = midiEvents.eventAt(i);
        if (e.payloadSize == 0 || e.payloadSize > VSTPOC_MIDI_PAYLOAD_RING_BYTES ||
            e.frameOffset >= blockFrames ||
            payloadHead - payloadTail + e.payloadSize > VSTPOC_MIDI_PAYLOAD_RING_BYTES) {
            __atomic_add_fetch(&data_->midi_input_drop_count, 1u, __ATOMIC_RELAXED);
            continue;
        }
        copyRingIn(data_->midi_input_payload, payloadHead, midiEvents.payloadFor(e), e.payloadSize);
        auto& d = b.midi_events[b.midi_event_count++];
        d.frame_offset = e.frameOffset; d.payload_size = e.payloadSize; d.payload_offset = payloadHead;
        payloadHead += e.payloadSize;
    }
    b.midi_payload_begin = payloadBegin; b.midi_payload_end = payloadHead;
    __atomic_store_n(&data_->midi_input_payload_head, payloadHead, __ATOMIC_RELEASE);
    const uint64_t deadlineBudgetNs = sampleRate > 0.0
        ? static_cast<uint64_t>(static_cast<double>(blockFrames) * 1'000'000'000.0 / sampleRate) : 0;
    __atomic_store_n(&data_->block_deadline_ns, deadlineBudgetNs, __ATOMIC_RELAXED);
    __atomic_store_n(&data_->transport_queue_head, qh + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&data_->transport_seq, qh + 2u, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&data_->wake_requested, 1u, __ATOMIC_ACQ_REL) == 0u) signalWake();
    return true;
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
    if (__atomic_exchange_n(&data_->wake_requested, 1u, __ATOMIC_ACQ_REL) == 0u)
        signalWake();
    return numFrames;
}



void SharedRing::setMicActive(bool active) {
    if (!data_) return;
    __atomic_store_n(&data_->mic_active, active ? 1 : 0, __ATOMIC_RELEASE);
}

void SharedRing::pushParam(int32_t index, float value) {
    if (!data_ || index < 0 || index >= static_cast<int32_t>(VSTPOC_MAX_PARAMS)) return;
    data_->param_desired_values[index] = value;
    __atomic_add_fetch(&data_->param_desired_seq[index], UINT64_C(1), __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&data_->wake_requested, 1u, __ATOMIC_ACQ_REL) == 0u)
        signalWake();
}

void SharedRing::signalStop() {
    if (!data_) return;
    __atomic_store_n(&data_->stop_flag, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&data_->wake_requested, 1u, __ATOMIC_ACQ_REL) == 0u)
        signalWake();
}

void SharedRing::notifyGuest() {
    if (data_ && __atomic_exchange_n(&data_->wake_requested, 1u, __ATOMIC_ACQ_REL) == 0u)
        signalWake();
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
uint64_t SharedRing::midiDropCount() const noexcept {
    return data_ ? __atomic_load_n(&data_->midi_input_drop_count, __ATOMIC_RELAXED) +
                   __atomic_load_n(&data_->midi_output_drop_count, __ATOMIC_RELAXED) : 0;
}
