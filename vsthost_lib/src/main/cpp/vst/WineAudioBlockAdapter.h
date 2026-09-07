#ifndef VSTHOST_WINE_AUDIO_BLOCK_ADAPTER_H
#define VSTHOST_WINE_AUDIO_BLOCK_ADAPTER_H

#include "../../../../../app/src/main/cpp/plugin/IPlugin.h"
#include "../ipc/SharedRing.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace vsthost {

/** Collects graph-sized callbacks into one stable guest block without RT allocation. */
class WineAudioBlockAdapter {
public:
    WineAudioBlockAdapter() = default;
    explicit WineAudioBlockAdapter(uint32_t graphFrames) { configure(graphFrames); }

    bool configure(uint32_t graphFrames) noexcept {
        reset();
        if (graphFrames == 0 || graphFrames > VSTPOC_MAX_BLOCK_FRAMES) return false;
        const uint64_t n = ((static_cast<uint64_t>(128) + graphFrames - 1) / graphFrames) * graphFrames;
        if (n > VSTPOC_MAX_BLOCK_FRAMES) return false;
        graphFrames_ = graphFrames;
        guestFrames_ = static_cast<uint32_t>(n);
        return true;
    }
    uint32_t accumulationLatencyFrames() const noexcept {
        return valid() ? guestFrames_ - graphFrames_ : 0;
    }
    bool appendInput(const float* left, const float* right, uint32_t frames,
                     const guitarrackcraft::AudioProcessContext& context,
                     const guitarrackcraft::MidiEvent* midi, uint32_t midiCount) noexcept {
        if (frames != graphFrames_) return false;
        const float* planes[2] = {left, right};
        return appendInput(planes, context, midi, midiCount);
    }
    void reset() noexcept {
        graphFrames_ = guestFrames_ = inputFrames_ = 0;
        outputReadIndex_ = outputWriteIndex_ = outputBlockCount_ = 0;
        outputSlice_ = 0;
        inputMidiCount_ = 0;
        midiDrops_ = 0;
    }
    constexpr bool valid() const noexcept { return graphFrames_ != 0; }
    uint32_t graphFrames() const noexcept { return graphFrames_; }
    uint32_t guestFrames() const noexcept { return guestFrames_; }
    static constexpr uint32_t outputSlotCapacity() noexcept { return kOutputSlots; }
    constexpr uint32_t outputReserveLatencyFrames() const noexcept {
        return valid() ? (outputSlotCapacity() - 1u) * guestFrames_ : 0;
    }

    bool appendInput(const float* const* inputs,
                     const guitarrackcraft::AudioProcessContext& context,
                     const guitarrackcraft::MidiEvent* midi, uint32_t midiCount) noexcept {
        if (!valid() || inputFrames_ + graphFrames_ > guestFrames_) return false;
        const float* left = (inputs && inputs[0]) ? inputs[0] : nullptr;
        const float* right = (inputs && inputs[1]) ? inputs[1] : left;
        float* dstL = input_[0].data() + inputFrames_;
        float* dstR = input_[1].data() + inputFrames_;
        if (left) std::memcpy(dstL, left, graphFrames_ * sizeof(float));
        else std::fill(dstL, dstL + graphFrames_, 0.0f);
        if (right) std::memcpy(dstR, right, graphFrames_ * sizeof(float));
        else std::fill(dstR, dstR + graphFrames_, 0.0f);
        if (inputFrames_ == 0) inputContext_ = context;
        const uint32_t base = inputFrames_;
        const uint32_t room = VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK - inputMidiCount_;
        const uint32_t take = midi ? std::min(midiCount, room) : 0;
        for (uint32_t i = 0; i < take; ++i) {
            inputMidi_[inputMidiCount_ + i] = midi[i];
            inputMidi_[inputMidiCount_ + i].frameOffset =
                base + std::min(midi[i].frameOffset, graphFrames_ - 1u);
        }
        midiDrops_ += midiCount - take;
        inputMidiCount_ += take;
        inputFrames_ += graphFrames_;
        return true;
    }
    bool inputReady() const noexcept { return inputFrames_ == guestFrames_ && valid(); }
    const float* inputLeft() const noexcept { return input_[0].data(); }
    const float* inputRight() const noexcept { return input_[1].data(); }
    const guitarrackcraft::AudioProcessContext& inputContext() const noexcept { return inputContext_; }
    const guitarrackcraft::MidiEvent* inputMidi() const noexcept { return inputMidi_.data(); }
    uint32_t inputMidiCount() const noexcept { return inputMidiCount_; }
    void consumeInput() noexcept { inputFrames_ = 0; inputMidiCount_ = 0; }

    float* outputLeft() noexcept { return output_[outputWriteIndex_][0].data(); }
    float* outputRight() noexcept { return output_[outputWriteIndex_][1].data(); }
    bool outputReady() const noexcept { return outputBlockCount_ != 0; }
    uint32_t outputBlockCount() const noexcept { return outputBlockCount_; }
    bool outputWriteAvailable() const noexcept {
        return valid() && outputBlockCount_ < kOutputSlots;
    }
    uint32_t outputFramesAvailable() const noexcept {
        return outputReady() ? guestFrames_ - outputSlice_ : 0;
    }
    uint32_t outputMidiCount() const noexcept {
        return outputReady() ? outputMidiCount_[outputReadIndex_] : 0;
    }
    bool commitOutput(const guitarrackcraft::MidiEvent* midi, uint32_t count) noexcept {
        if (!valid() || !outputWriteAvailable()) return false;
        const uint32_t slot = outputWriteIndex_;
        const uint32_t take = midi
            ? std::min(count, static_cast<uint32_t>(VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK))
            : 0;
        outputMidiCount_[slot] = take;
        if (midi && take && midi != outputMidi_[slot].data())
            std::memcpy(outputMidi_[slot].data(), midi, take * sizeof(midi[0]));
        midiDrops_ += count - take;
        outputWriteIndex_ = (outputWriteIndex_ + 1u) % kOutputSlots;
        ++outputBlockCount_;
        return true;
    }
    guitarrackcraft::MidiEvent* outputMidi() noexcept {
        return outputMidi_[outputWriteIndex_].data();
    }
    uint32_t copyOutput(float* outL, float* outR,
                        guitarrackcraft::MidiEvent* events, uint32_t capacity) noexcept {
        if (!outputReady()) return 0;
        const uint32_t slot = outputReadIndex_;
        const uint32_t begin = outputSlice_;
        const uint32_t end = begin + graphFrames_;
        if (outL) std::memcpy(outL, output_[slot][0].data() + begin, graphFrames_ * sizeof(float));
        if (outR) std::memcpy(outR, output_[slot][1].data() + begin, graphFrames_ * sizeof(float));
        uint32_t written = 0;
        for (uint32_t i = 0; i < outputMidiCount_[slot]; ++i) {
            const auto& e = outputMidi_[slot][i];
            if (e.frameOffset < begin || e.frameOffset >= end) continue;
            if (events && written < capacity) {
                events[written] = e;
                events[written++].frameOffset -= begin;
            } else {
                ++midiDrops_;
            }
        }
        outputSlice_ = end;
        if (outputSlice_ >= guestFrames_) {
            outputSlice_ = 0;
            outputMidiCount_[slot] = 0;
            outputReadIndex_ = (outputReadIndex_ + 1u) % kOutputSlots;
            --outputBlockCount_;
        }
        return written;
    }
    uint64_t midiDropCount() const noexcept { return midiDrops_; }

private:
    static constexpr uint32_t kOutputSlots = 3;
    uint32_t graphFrames_ = 0, guestFrames_ = 0, inputFrames_ = 0;
    uint32_t outputReadIndex_ = 0, outputWriteIndex_ = 0;
    uint32_t outputBlockCount_ = 0, outputSlice_ = 0, inputMidiCount_ = 0;
    std::array<uint32_t, kOutputSlots> outputMidiCount_{};
    guitarrackcraft::AudioProcessContext inputContext_{};
    std::array<float, VSTPOC_MAX_BLOCK_FRAMES> input_[2]{};
    std::array<float, VSTPOC_MAX_BLOCK_FRAMES> output_[kOutputSlots][2]{};
    std::array<guitarrackcraft::MidiEvent, VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK>
        inputMidi_{};
    std::array<guitarrackcraft::MidiEvent, VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK>
        outputMidi_[kOutputSlots]{};
    uint64_t midiDrops_ = 0;
};

} // namespace vsthost
#endif
