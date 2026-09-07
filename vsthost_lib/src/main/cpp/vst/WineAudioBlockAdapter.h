#ifndef VSTHOST_WINE_AUDIO_BLOCK_ADAPTER_H
#define VSTHOST_WINE_AUDIO_BLOCK_ADAPTER_H
#include "../../../../../app/src/main/cpp/plugin/IPlugin.h"
#include "../ipc/SharedRing.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
namespace vsthost {
class WineFailClosedAudioState {
public:
    void reset() noexcept {
        fallback_ = true;
        haveLast_ = false;
        decaySamples_ = 0;
        recoverySamples_ = 0;
        lastLeft_ = 0.0f;
        lastRight_ = 0.0f;
    }
    // Render only bounded decay of the last processed sample, then silence.
    // The current input is intentionally not accepted by this API.
    void renderFailure(float* const* outputs, uint32_t frames) noexcept {
        if (!fallback_ || recoverySamples_ != 0) decaySamples_ = 0;
        float left = haveLast_ ? lastLeft_ : 0.0f;
        float right = haveLast_ ? lastRight_ : 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            if (decaySamples_ < 64) ++decaySamples_;
            const float gain = 1.0f - static_cast<float>(decaySamples_) / 64.0f;
            left *= gain;
            right *= gain;
            if (outputs && outputs[0]) outputs[0][i] = left;
            if (outputs && outputs[1]) outputs[1][i] = right;
        }
        if (frames != 0) {
            lastLeft_ = left;
            lastRight_ = right;
            haveLast_ = true;
        }
        fallback_ = true;
        recoverySamples_ = 0;
    }
    // Fade processed output in from zero after recovery; never reads input.
    void applyRecovery(float* const* outputs, uint32_t frames) noexcept {
        if (!fallback_) return;
        for (uint32_t i = 0; i < frames && recoverySamples_ < 64; ++i) {
            ++recoverySamples_;
            const float gain = static_cast<float>(recoverySamples_) / 64.0f;
            if (outputs && outputs[0]) outputs[0][i] *= gain;
            if (outputs && outputs[1]) outputs[1][i] *= gain;
        }
        if (recoverySamples_ >= 64) {
            fallback_ = false;
            decaySamples_ = 0;
            recoverySamples_ = 0;
        }
    }
    void rememberProcessed(const float* left, const float* right, uint32_t frames) noexcept {
        if (frames == 0 || !left || !right) return;
        lastLeft_ = left[frames - 1];
        lastRight_ = right[frames - 1];
        haveLast_ = true;
    }
private:
    bool fallback_ = true;
    bool haveLast_ = false;
    uint32_t decaySamples_ = 0;
    uint32_t recoverySamples_ = 0;
    float lastLeft_ = 0.0f;
    float lastRight_ = 0.0f;
};

class WineAudioBlockAdapter {
public:
    static constexpr bool acceptsCallbackFrames(uint32_t capacity,
                                                uint32_t frames) noexcept {
        return frames != 0 && frames <= capacity &&
               frames <= VSTPOC_MAX_BLOCK_FRAMES;
    }
    WineAudioBlockAdapter() = default;
    explicit WineAudioBlockAdapter(uint32_t graphFrames) { configure(graphFrames); }
    bool configure(uint32_t graphFrames) noexcept {
        reset(); if (graphFrames == 0 || graphFrames > VSTPOC_MAX_BLOCK_FRAMES) return false;
        const uint64_t n = ((static_cast<uint64_t>(128) + graphFrames - 1) / graphFrames) * graphFrames;
        if (n > VSTPOC_MAX_BLOCK_FRAMES) return false; graphFrames_ = graphFrames; guestFrames_ = static_cast<uint32_t>(n); return true;
    }
    uint32_t accumulationLatencyFrames() const noexcept { return valid() ? guestFrames_ - graphFrames_ : 0; }
    bool appendInput(const float* const* inputs, const guitarrackcraft::AudioProcessContext& context,
                     const guitarrackcraft::MidiBuffer& midi) noexcept {
        if (!valid() || inputFrames_ + graphFrames_ > guestFrames_) return false;
        const float* left = (inputs && inputs[0]) ? inputs[0] : nullptr;
        const float* right = (inputs && inputs[1]) ? inputs[1] : left;
        float* dstL = input_[0].data() + inputFrames_; float* dstR = input_[1].data() + inputFrames_;
        if (left) std::memcpy(dstL, left, graphFrames_ * sizeof(float)); else std::fill(dstL, dstL + graphFrames_, 0.0f);
        if (right) std::memcpy(dstR, right, graphFrames_ * sizeof(float)); else std::fill(dstR, dstR + graphFrames_, 0.0f);
        if (inputFrames_ == 0) inputContext_ = context;
        const uint32_t base = inputFrames_;
        for (uint32_t i = 0; i < midi.eventCount(); ++i) {
            const auto& e = midi.eventAt(i);
            if (!inputMidi_.append(base + std::min(e.frameOffset, graphFrames_ - 1u),
                                   midi.payloadFor(e), e.payloadSize)) ++midiDrops_;
        }
        inputFrames_ += graphFrames_; return true;
    }
    void reset() noexcept {
        graphFrames_ = guestFrames_ = inputFrames_ = 0;
        outputReadIndex_ = outputWriteIndex_ = outputBlockCount_ = outputSlice_ = 0;
        inputMidi_.clear(); for (auto& midi : outputMidi_) midi.clear();
        for (auto& flag : outputAuthoritative_) flag = false;
        midiDrops_ = 0;
    }
    constexpr bool valid() const noexcept { return graphFrames_ != 0; }
    uint32_t graphFrames() const noexcept { return graphFrames_; }
    uint32_t guestFrames() const noexcept { return guestFrames_; }
    static constexpr uint32_t outputSlotCapacity() noexcept { return kOutputSlots; }
    constexpr uint32_t outputReserveLatencyFrames() const noexcept { return valid() ? (kOutputSlots - 1u) * guestFrames_ : 0; }
    bool inputReady() const noexcept { return inputFrames_ == guestFrames_ && valid(); }
    const float* inputLeft() const noexcept { return input_[0].data(); }
    const float* inputRight() const noexcept { return input_[1].data(); }
    const guitarrackcraft::AudioProcessContext& inputContext() const noexcept { return inputContext_; }
    const guitarrackcraft::MidiBuffer& inputMidi() const noexcept { return inputMidi_; }
    void consumeInput() noexcept { inputFrames_ = 0; inputMidi_.clear(); }
    float* outputLeft() noexcept { return output_[outputWriteIndex_][0].data(); }
    float* outputRight() noexcept { return output_[outputWriteIndex_][1].data(); }
    bool outputReady() const noexcept { return outputBlockCount_ != 0; }
    uint32_t outputBlockCount() const noexcept { return outputBlockCount_; }
    bool outputAuthoritative() const noexcept {
        return outputReady() && outputAuthoritative_[outputReadIndex_];
    }
    bool outputWriteAvailable() const noexcept { return valid() && outputBlockCount_ < kOutputSlots; }
    guitarrackcraft::MidiBuffer& outputMidi() noexcept { return outputMidi_[outputWriteIndex_]; }
    bool commitOutput(const guitarrackcraft::MidiBuffer& midi, bool authoritative = false) noexcept {
        if (!valid() || !outputWriteAvailable()) return false;
        auto& dst = outputMidi_[outputWriteIndex_]; dst.clear();
        for (uint32_t i = 0; i < midi.eventCount(); ++i) {
            const auto& e = midi.eventAt(i);
            if (!dst.append(e.frameOffset, midi.payloadFor(e), e.payloadSize)) ++midiDrops_;
        }
        outputAuthoritative_[outputWriteIndex_] = authoritative;
        outputWriteIndex_ = (outputWriteIndex_ + 1u) % kOutputSlots; ++outputBlockCount_; return true;
    }
    uint32_t copyOutput(float* outL, float* outR, guitarrackcraft::MidiBuffer& midi) noexcept {
        if (!outputReady()) return 0;
        const uint32_t slot = outputReadIndex_, begin = outputSlice_, end = begin + graphFrames_;
        if (outL) std::memcpy(outL, output_[slot][0].data() + begin, graphFrames_ * sizeof(float));
        if (outR) std::memcpy(outR, output_[slot][1].data() + begin, graphFrames_ * sizeof(float));
        uint32_t written = 0;
        for (uint32_t i = 0; i < outputMidi_[slot].eventCount(); ++i) {
            const auto& e = outputMidi_[slot].eventAt(i);
            if (e.frameOffset < begin || e.frameOffset >= end) continue;
            if (midi.append(
                    e.frameOffset - begin,
                    outputMidi_[slot].payloadFor(e),
                    e.payloadSize)) {
                ++written;
            }
        }
        outputSlice_ = end;
        if (outputSlice_ >= guestFrames_) {
            outputSlice_ = 0; outputMidi_[slot].clear();
            outputReadIndex_ = (outputReadIndex_ + 1u) % kOutputSlots; --outputBlockCount_;
        }
        return written;
    }
    uint64_t midiDropCount() const noexcept { return midiDrops_; }
private:
    static constexpr uint32_t kOutputSlots = 3;
    uint32_t graphFrames_ = 0, guestFrames_ = 0, inputFrames_ = 0, outputReadIndex_ = 0, outputWriteIndex_ = 0, outputBlockCount_ = 0, outputSlice_ = 0;
    guitarrackcraft::AudioProcessContext inputContext_{};
    std::array<float, VSTPOC_MAX_BLOCK_FRAMES> input_[2]{};
    std::array<float, VSTPOC_MAX_BLOCK_FRAMES> output_[kOutputSlots][2]{};
    guitarrackcraft::MidiBuffer inputMidi_{};
    uint64_t midiDrops_ = 0;
    std::array<guitarrackcraft::MidiBuffer, kOutputSlots> outputMidi_{};
    std::array<bool, kOutputSlots> outputAuthoritative_{};
};
}
#endif
