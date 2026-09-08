#include <gtest/gtest.h>

#include <algorithm>

#include "plugin/LiveMidiSource.h"
#include "plugin/RackGraph.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace {
using guitarrackcraft::IPlugin;
using guitarrackcraft::LiveMidiSource;
using guitarrackcraft::MidiBuffer;
using guitarrackcraft::MidiOutputDisposition;
using guitarrackcraft::PluginInfo;
using guitarrackcraft::RealtimeClass;
using guitarrackcraft::RackGraph;
using guitarrackcraft::RackPathId;
using guitarrackcraft::UsbMidiPortIdentity;

uint64_t monotonicNowNanos() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

class MidiCapturePlugin final : public IPlugin {
public:
    struct Event {
        uint32_t frame = 0;
        uint32_t size = 0;
        std::array<uint8_t, 256> bytes{};
    };

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    MidiOutputDisposition process(
            const float* const* inputs, float* const* outputs,
            uint32_t numFrames, const guitarrackcraft::AudioProcessContext&,
            const MidiBuffer& inputMidi, MidiBuffer&) override {
        for (uint32_t index = 0;
             index < inputMidi.eventCount() && count_ < events_.size(); ++index) {
            const auto& input = inputMidi.eventAt(index);
            auto& output = events_[count_++];
            output.frame = input.frameOffset;
            output.size = input.payloadSize;
            if (input.payloadSize <= output.bytes.size()) {
                std::copy_n(inputMidi.payloadFor(input), input.payloadSize,
                            output.bytes.begin());
            }
        }
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return MidiOutputDisposition::Passthrough;
    }

    PluginInfo getInfo() const override {
        PluginInfo info;
        info.realtimeClass = RealtimeClass::CertifiedInProcess;
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

    uint32_t count() const { return count_; }
    const Event& event(uint32_t index) const { return events_[index]; }
    void clear() { count_ = 0; }

private:
    std::array<Event, guitarrackcraft::kMaxMidiEvents> events_{};
    uint32_t count_ = 0;
};

struct AudioBuffers {
    std::array<float, 512> left{};
    std::array<float, 512> right{};
    std::array<float, 512> outputLeft{};
    std::array<float, 512> outputRight{};
    const float* inputs[2] = {left.data(), right.data()};
    float* outputs[2] = {outputLeft.data(), outputRight.data()};
};

UsbMidiPortIdentity testIdentity() {
    UsbMidiPortIdentity identity;
    identity.vendorId = 0x1234;
    identity.productId = 0x5678;
    identity.serialNumber = "serial-1";
    identity.portNumber = 2;
    return identity;
}

void configureGraph(RackGraph& graph) {
    // The long block leaves ample room for a timestamp sampled immediately
    // before process() while keeping all frame mapping observable.
    graph.setSampleRate(60.0f, 512);
}

uint64_t enqueueTimestamp() {
    return monotonicNowNanos() - 1'000'000;
}

} // namespace

TEST(LiveMidiSourceTest, PreservesShortAndExactMaximumPayloads) {
    const UsbMidiPortIdentity identity = testIdentity();
    LiveMidiSource source(1, identity);

    const std::array<uint8_t, 3> shortMessage = {0x90, 60, 100};
    const uint64_t timestamp = 10'000'000'000;
    const uint32_t offset = 0;
    const uint32_t length = static_cast<uint32_t>(shortMessage.size());
    ASSERT_EQ(source.enqueue(&timestamp, &offset, &length,
                             shortMessage.data(), 1), 1u);

    MidiBuffer shortOutput;
    uint32_t lateDrops = 0;
    uint32_t otherDrops = 0;
    ASSERT_EQ(source.drain(128, timestamp + 1, 48'000.0, shortOutput,
                           [&](bool late) {
                               if (late) ++lateDrops;
                               else ++otherDrops;
                           }), 1u);
    ASSERT_EQ(shortOutput.eventCount(), 1u);
    ASSERT_EQ(shortOutput.eventAt(0).frameOffset, 127u);
    ASSERT_EQ(shortOutput.eventAt(0).payloadSize, shortMessage.size());
    EXPECT_TRUE(std::equal(shortMessage.begin(), shortMessage.end(),
                           shortOutput.payloadFor(shortOutput.eventAt(0))));
    EXPECT_EQ(lateDrops, 0u);
    EXPECT_EQ(otherDrops, 0u);

    std::vector<uint8_t> sysex(guitarrackcraft::kMaxMidiPayloadBytes);
    for (uint32_t index = 0; index < sysex.size(); ++index) {
        sysex[index] = static_cast<uint8_t>(index % 251);
    }
    const uint32_t sysexLength = guitarrackcraft::kMaxMidiPayloadBytes;
    ASSERT_EQ(source.enqueue(&timestamp, &offset, &sysexLength,
                             sysex.data(), 1), 1u);

    MidiBuffer sysexOutput;
    ASSERT_EQ(source.drain(128, timestamp + 1, 48'000.0, sysexOutput,
                           [&](bool late) { if (late) ++lateDrops; else ++otherDrops; }), 1u);
    ASSERT_EQ(sysexOutput.eventCount(), 1u);
    ASSERT_EQ(sysexOutput.eventAt(0).payloadSize, sysex.size());
    EXPECT_TRUE(std::equal(sysex.begin(), sysex.end(),
                           sysexOutput.payloadFor(sysexOutput.eventAt(0))));
    EXPECT_EQ(lateDrops, 0u);
    EXPECT_EQ(otherDrops, 0u);
}
TEST(LiveMidiSourceTest, DrainsMessagesInFrameOrderWithStableEqualTimestampOrder) {
    LiveMidiSource source(2, testIdentity());
    const uint64_t now = 1'000'000'000;
    const uint32_t offsets[] = {0, 3, 6};
    const uint32_t lengths[] = {3, 3, 3};
    const std::array<uint8_t, 9> payload = {
        0x90, 60, 100, 0x90, 61, 101, 0x90, 62, 102,
    };
    // The first event is latest, while the next two share an earlier
    // timestamp. Sorting must reorder by frame and retain their enqueue
    // order at the equal frame.
    const uint64_t timestamps[] = {
        now - 20'000'000, now - 80'000'000, now - 80'000'000,
    };
    ASSERT_EQ(source.enqueue(timestamps, offsets, lengths, payload.data(), 3), 3u);

    MidiBuffer output;
    EXPECT_EQ(source.drain(100, now, 1'000.0, output,
                           [&](bool) { FAIL() << "test event was dropped"; }), 3u);
    ASSERT_EQ(output.eventCount(), 3u);
    EXPECT_EQ(output.eventAt(0).frameOffset, 20u);
    EXPECT_EQ(output.eventAt(1).frameOffset, 20u);
    EXPECT_EQ(output.eventAt(2).frameOffset, 80u);
    EXPECT_EQ(output.payloadFor(output.eventAt(0))[1], 61u);
    EXPECT_EQ(output.payloadFor(output.eventAt(1))[1], 62u);
    EXPECT_EQ(output.payloadFor(output.eventAt(2))[1], 60u);
}


TEST(LiveMidiSourceTest, RejectsNewestMessageWhenDescriptorRingIsFull) {
    LiveMidiSource source(2, testIdentity());
    std::array<uint64_t, LiveMidiSource::kDescriptorCapacity> timestamps{};
    std::array<uint32_t, LiveMidiSource::kDescriptorCapacity> offsets{};
    std::array<uint32_t, LiveMidiSource::kDescriptorCapacity> lengths{};
    std::array<uint8_t, LiveMidiSource::kDescriptorCapacity> payload{};
    for (uint32_t index = 0; index < LiveMidiSource::kDescriptorCapacity; ++index) {
        timestamps[index] = 1'000'000 + index;
        lengths[index] = 1;
        payload[index] = static_cast<uint8_t>(index);
    }
    ASSERT_EQ(source.enqueue(timestamps.data(), offsets.data(), lengths.data(),
                             payload.data(), LiveMidiSource::kDescriptorCapacity),
              LiveMidiSource::kDescriptorCapacity);

    const uint64_t extraTimestamp = 2'000'000;
    const uint32_t extraOffset = 0;
    const uint32_t extraLength = 1;
    const uint8_t extraPayload = 0xee;
    EXPECT_EQ(source.enqueue(&extraTimestamp, &extraOffset, &extraLength,
                             &extraPayload, 1), 0u);
    EXPECT_EQ(source.dropped(), 1u);
}

TEST(LiveMidiSourceTest, RejectsNewestMessageWhenPayloadRingIsFull) {
    LiveMidiSource source(3, testIdentity());
    constexpr uint32_t messageSize = guitarrackcraft::kMaxMidiPayloadBytes;
    std::vector<uint8_t> payload(messageSize, 0x5a);
    const uint32_t offset = 0;
    const uint32_t length = messageSize;
    for (uint64_t timestamp = 1; timestamp <= 4; ++timestamp) {
        ASSERT_EQ(source.enqueue(&timestamp, &offset, &length,
                                 payload.data(), 1), 1u);
    }

    const uint64_t timestamp = 5;
    const uint32_t shortLength = 1;
    const uint8_t byte = 0x7f;
    EXPECT_EQ(source.enqueue(&timestamp, &offset, &shortLength, &byte, 1), 0u);
    EXPECT_EQ(source.dropped(), 1u);
}

TEST(LiveMidiSourceTest, PublishesAndReadsPayloadAfterRingWrap) {
    LiveMidiSource source(4, testIdentity());
    // Four non-divisor chunks leave the empty ring at offset 260000, close
    // enough to the end that the next 64-KiB message must wrap to offset 0.
    constexpr uint32_t chunk = 65'000;
    std::vector<uint8_t> initial(chunk, 0x31);
    const uint32_t offset = 0;
    const uint32_t length = chunk;
    for (uint64_t timestamp = 1; timestamp <= 4; ++timestamp) {
        const uint64_t callbackTimestamp = 1'000'000 + timestamp;
        ASSERT_EQ(source.enqueue(&callbackTimestamp, &offset, &length,
                                 initial.data(), 1), 1u);
    }
    MidiBuffer discardedOutput;
    uint32_t discarded = 0;
    ASSERT_EQ(source.drain(128, 2'000'000, 48'000.0, discardedOutput,
                           [&](bool) { ++discarded; }), 4u);
    EXPECT_EQ(discarded, 3u);

    std::vector<uint8_t> wrapped(guitarrackcraft::kMaxMidiPayloadBytes);
    for (uint32_t index = 0; index < wrapped.size(); ++index) {
        wrapped[index] = static_cast<uint8_t>((index * 7) % 251);
    }
    const uint64_t timestamp = 2'000'000;
    const uint32_t wrappedLength = guitarrackcraft::kMaxMidiPayloadBytes;
    ASSERT_EQ(source.enqueue(&timestamp, &offset, &wrappedLength,
                             wrapped.data(), 1), 1u);

    MidiBuffer output;
    ASSERT_EQ(source.drain(128, timestamp + 1, 48'000.0, output,
                           [&](bool) { FAIL() << "wrapped event was dropped"; }), 1u);
    ASSERT_EQ(output.eventCount(), 1u);
    EXPECT_EQ(output.eventAt(0).payloadSize, wrapped.size());
    EXPECT_TRUE(std::equal(wrapped.begin(), wrapped.end(),
                           output.payloadFor(output.eventAt(0))));
}

TEST(LiveMidiSourceTest, EpochFlushConsumesQueuedStaleMessages) {
    LiveMidiSource source(5, testIdentity());
    const std::array<uint8_t, 3> stale = {0x90, 64, 127};
    const uint64_t timestamp = 3'000'000;
    const uint32_t offset = 0;
    const uint32_t length = 3;
    ASSERT_EQ(source.enqueue(&timestamp, &offset, &length, stale.data(), 1), 1u);
    const uint32_t oldEpoch = source.epoch();
    source.flush();
    EXPECT_EQ(source.epoch(), oldEpoch + 1);

    MidiBuffer output;
    uint32_t callbacks = 0;
    EXPECT_EQ(source.drain(64, timestamp + 1, 48'000.0, output,
                           [&](bool) { ++callbacks; }), 1u);
    EXPECT_EQ(output.eventCount(), 0u);
    EXPECT_EQ(callbacks, 0u);

    const std::array<uint8_t, 3> current = {0x80, 64, 0};
    ASSERT_EQ(source.enqueue(&timestamp, &offset, &length, current.data(), 1), 1u);
    EXPECT_EQ(source.drain(64, timestamp + 1, 48'000.0, output,
                           [&](bool) { FAIL() << "current event was dropped"; }), 1u);
    ASSERT_EQ(output.eventCount(), 1u);
    EXPECT_TRUE(std::equal(current.begin(), current.end(),
                           output.payloadFor(output.eventAt(0))));
}

TEST(RackGraphMidiIngressTest, LateNoteOffMapsToFrameZeroAndOnlyLateCounterChanges) {
    RackGraph graph;
    configureGraph(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    const auto identity = testIdentity();
    const uint64_t handle = graph.registerUsbMidiSource(identity);
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(graph.setTrackMidiInputUsb(track, identity, "USB", handle));

    auto capture = std::make_unique<MidiCapturePlugin>();
    auto* capturePtr = capture.get();
    ASSERT_EQ(graph.getChain(track)->addPlugin(std::move(capture)), 0);
    AudioBuffers buffers;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    capturePtr->clear();

    const std::array<uint8_t, 3> noteOff = {0x80, 60, 0};
    const uint64_t timestamp = 1;
    const uint32_t offset = 0;
    const uint32_t length = 3;
    ASSERT_EQ(graph.enqueueUsbMidiBatch(handle, &timestamp, &offset, &length,
                                        noteOff.data(), 1, 0, 0), 1u);

    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    ASSERT_EQ(capturePtr->count(), 1u);
    EXPECT_EQ(capturePtr->event(0).frame, 0u);
    EXPECT_EQ(capturePtr->event(0).size, noteOff.size());
    EXPECT_TRUE(std::equal(noteOff.begin(), noteOff.end(),
                           capturePtr->event(0).bytes.begin()));
    EXPECT_EQ(graph.getMidiLateEvents(), 1u);
    EXPECT_EQ(graph.getMidiIngressDrops(), 0u);
    EXPECT_EQ(graph.getMidiOversizeMessages(), 0u);
    EXPECT_EQ(graph.getMidiMalformedMessages(), 0u);
    EXPECT_EQ(graph.getMidiMergeDrops(), 0u);
    EXPECT_EQ(graph.getMidiPluginOutputDrops(), 0u);
}

TEST(RackGraphMidiIngressTest, QuantumOverflowIsConsumedAndCountsIngressDrop) {
    RackGraph graph;
    configureGraph(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    const auto identity = testIdentity();
    const uint64_t handle = graph.registerUsbMidiSource(identity);
    ASSERT_TRUE(graph.setTrackMidiInputUsb(track, identity, "USB", handle));

    auto capture = std::make_unique<MidiCapturePlugin>();
    auto* capturePtr = capture.get();
    ASSERT_EQ(graph.getChain(track)->addPlugin(std::move(capture)), 0);
    AudioBuffers buffers;
    graph.process(buffers.inputs, 2, buffers.outputs, 512);
    capturePtr->clear();

    std::array<uint64_t, guitarrackcraft::kMaxMidiEvents + 1> timestamps{};
    std::array<uint32_t, guitarrackcraft::kMaxMidiEvents + 1> offsets{};
    std::array<uint32_t, guitarrackcraft::kMaxMidiEvents + 1> lengths{};
    std::array<uint8_t, (guitarrackcraft::kMaxMidiEvents + 1) * 3> payload{};
    const uint64_t timestamp = enqueueTimestamp();
    for (uint32_t index = 0; index < timestamps.size(); ++index) {
        timestamps[index] = timestamp;
        offsets[index] = index * 3;
        lengths[index] = 3;
        payload[index * 3] = 0x90;
        payload[index * 3 + 1] = static_cast<uint8_t>(index);
        payload[index * 3 + 2] = 100;
    }
    ASSERT_EQ(graph.enqueueUsbMidiBatch(handle, timestamps.data(), offsets.data(),
                                        lengths.data(), payload.data(),
                                        timestamps.size(), 0, 0),
              timestamps.size());

    graph.process(buffers.inputs, 2, buffers.outputs, 512);

    ASSERT_EQ(capturePtr->count(), guitarrackcraft::kMaxMidiEvents);
    EXPECT_EQ(capturePtr->event(guitarrackcraft::kMaxMidiEvents - 1).bytes[1], 127u);
    EXPECT_EQ(graph.getMidiIngressDrops(), 1u);
    EXPECT_EQ(graph.getMidiLateEvents(), 0u);
    EXPECT_EQ(graph.getMidiMergeDrops(), 0u);
}

TEST(RackGraphMidiIngressTest, USBBindingRejectsIdentityMismatch) {
    RackGraph graph;
    configureGraph(graph);
    const RackPathId track = graph.getTracks().front().id;
    const auto registered = testIdentity();
    const uint64_t handle = graph.registerUsbMidiSource(registered);
    ASSERT_NE(handle, 0u);

    auto wrong = registered;
    wrong.serialNumber = "different";
    EXPECT_FALSE(graph.setTrackMidiInputUsb(track, wrong, "wrong", handle));
    EXPECT_EQ(graph.getTrackMidiInputSource(track).kind,
              guitarrackcraft::TrackMidiInputSource::Kind::None);
    EXPECT_TRUE(graph.setTrackMidiInputUsb(track, registered, "USB", handle));
}
TEST(RackGraphMidiIngressTest, UsbIdentityAcceptsCodecBoundaryAndRejectsOverlongStrings) {
    RackGraph graph;
    configureGraph(graph);
    const RackPathId track = graph.getTracks().front().id;
    const auto boundaryString = [] {
        std::string value;
        value.reserve(48 * 6);
        for (int index = 0; index < 48; ++index) {
            // Modified UTF-8 surrogate pair: six bytes, one validator codepoint.
            value.append("\xed\xa0\xbd\xed\xb8\x80", 6);
        }
        return value;
    };
    auto identity = testIdentity();
    identity.serialNumber = boundaryString();
    const std::string displayName = boundaryString();
    const uint64_t handle = graph.registerUsbMidiSource(identity);
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(graph.setTrackMidiInputUsb(track, identity, displayName, handle));
    const auto accepted = graph.getTrackMidiInputSource(track);
    EXPECT_EQ(accepted.kind, guitarrackcraft::TrackMidiInputSource::Kind::UsbPort);
    EXPECT_EQ(accepted.usb.serialNumber.size(), 288u);
    EXPECT_EQ(accepted.displayName.size(), 288u);

    auto overlongIdentity = identity;
    overlongIdentity.serialNumber.push_back('X');
    EXPECT_FALSE(graph.setTrackMidiInputUsb(
        track, overlongIdentity, displayName, 0));
    std::string overlongDisplayName = displayName;
    overlongDisplayName.push_back('X');
    EXPECT_FALSE(graph.setTrackMidiInputUsb(
        track, identity, overlongDisplayName, 0));
    const auto preserved = graph.getTrackMidiInputSource(track);
    EXPECT_EQ(preserved.usb, identity);
    EXPECT_EQ(preserved.displayName, displayName);
}


TEST(RackGraphMidiIngressTest, OneRegisteredSourceFansOutOnceToTwoArmedTracks) {
    RackGraph graph;
    configureGraph(graph);

    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(first, second);
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    const auto identity = testIdentity();
    const uint64_t handle = graph.registerUsbMidiSource(identity);
    ASSERT_TRUE(graph.setTrackMidiInputUsb(first, identity, "USB", handle));
    ASSERT_TRUE(graph.setTrackMidiInputUsb(second, identity, "USB", handle));

    auto firstCapture = std::make_unique<MidiCapturePlugin>();
    auto* firstCapturePtr = firstCapture.get();
    ASSERT_EQ(graph.getChain(first)->addPlugin(std::move(firstCapture)), 0);
    auto secondCapture = std::make_unique<MidiCapturePlugin>();
    auto* secondCapturePtr = secondCapture.get();
    ASSERT_EQ(graph.getChain(second)->addPlugin(std::move(secondCapture)), 0);

    AudioBuffers buffers;
    graph.process(buffers.inputs, 2, buffers.outputs, 512);
    firstCapturePtr->clear();
    secondCapturePtr->clear();

    const std::array<uint8_t, 3> message = {0xb0, 7, 99};
    const uint64_t timestamp = enqueueTimestamp();
    const uint32_t offset = 0;
    const uint32_t length = 3;
    ASSERT_EQ(graph.enqueueUsbMidiBatch(handle, &timestamp, &offset, &length,
                                        message.data(), 1, 0, 0), 1u);
    graph.process(buffers.inputs, 2, buffers.outputs, 512);

    ASSERT_EQ(firstCapturePtr->count(), 1u);
    ASSERT_EQ(secondCapturePtr->count(), 1u);
    EXPECT_EQ(firstCapturePtr->event(0).size, message.size());
    EXPECT_EQ(secondCapturePtr->event(0).size, message.size());
    EXPECT_TRUE(std::equal(message.begin(), message.end(),
                           firstCapturePtr->event(0).bytes.begin()));
    EXPECT_TRUE(std::equal(message.begin(), message.end(),
                           secondCapturePtr->event(0).bytes.begin()));
    EXPECT_EQ(graph.getMidiIngressDrops(), 0u);
}
TEST(RackGraphMidiIngressTest, RegisteredHandlesAreNonzeroAndNeverReused) {
    RackGraph graph;
    const uint64_t first = graph.registerUsbMidiSource(testIdentity());
    ASSERT_NE(first, 0u);
    graph.unregisterUsbMidiSource(first);
    const uint64_t second = graph.registerUsbMidiSource(testIdentity());
    EXPECT_NE(second, 0u);
    EXPECT_GT(second, first);
}


TEST(RackGraphMidiIngressTest, UnregisteredHandleIsIgnored) {
    RackGraph graph;
    configureGraph(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    const auto identity = testIdentity();
    const uint64_t handle = graph.registerUsbMidiSource(identity);
    // Leave the source unbound so unregister cannot enqueue a panic into this
    // capture chain; this test isolates stale producer handles.
    auto capture = std::make_unique<MidiCapturePlugin>();
    auto* capturePtr = capture.get();
    ASSERT_EQ(graph.getChain(track)->addPlugin(std::move(capture)), 0);

    AudioBuffers buffers;
    graph.process(buffers.inputs, 2, buffers.outputs, 512);
    capturePtr->clear();

    graph.unregisterUsbMidiSource(handle);
    const std::array<uint8_t, 3> message = {0x90, 60, 100};
    const uint64_t timestamp = enqueueTimestamp();
    const uint32_t offset = 0;
    const uint32_t length = 3;
    EXPECT_EQ(graph.enqueueUsbMidiBatch(handle, &timestamp, &offset, &length,
                                        message.data(), 1, 0, 0), 0u);

    graph.process(buffers.inputs, 2, buffers.outputs, 512);
    EXPECT_EQ(capturePtr->count(), 0u);
}

TEST(RackGraphMidiIngressTest, GraphFlushDiscardsQueuedStaleDataBeforeNextQuantum) {
    RackGraph graph;
    configureGraph(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    const auto identity = testIdentity();
    const uint64_t handle = graph.registerUsbMidiSource(identity);
    ASSERT_TRUE(graph.setTrackMidiInputUsb(track, identity, "USB", handle));

    auto capture = std::make_unique<MidiCapturePlugin>();
    auto* capturePtr = capture.get();
    ASSERT_EQ(graph.getChain(track)->addPlugin(std::move(capture)), 0);
    AudioBuffers buffers;
    graph.process(buffers.inputs, 2, buffers.outputs, 512);
    capturePtr->clear();

    const std::array<uint8_t, 3> stale = {0x90, 62, 100};
    const uint64_t staleTimestamp = enqueueTimestamp();
    const uint32_t offset = 0;
    const uint32_t length = 3;
    ASSERT_EQ(graph.enqueueUsbMidiBatch(handle, &staleTimestamp, &offset, &length,
                                        stale.data(), 1, 0, 0), 1u);
    graph.flushUsbMidiSource(handle);
    graph.process(buffers.inputs, 2, buffers.outputs, 512);
    capturePtr->clear();

    const std::array<uint8_t, 3> current = {0x80, 62, 0};
    const uint64_t currentTimestamp = enqueueTimestamp();
    ASSERT_EQ(graph.enqueueUsbMidiBatch(handle, &currentTimestamp, &offset, &length,
                                        current.data(), 1, 0, 0), 1u);
    graph.process(buffers.inputs, 2, buffers.outputs, 512);

    ASSERT_EQ(capturePtr->count(), 1u);
    EXPECT_TRUE(std::equal(current.begin(), current.end(),
                           capturePtr->event(0).bytes.begin()));
}
