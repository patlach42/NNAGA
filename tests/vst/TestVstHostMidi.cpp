#include <gtest/gtest.h>

extern "C" {
#include "../../vsthost_lib/external/vst_host/vst_host_midi.h"
}
#include "../../vsthost_lib/external/vst_host_vst3/vst3_midi_conversion.h"

#include <cstdint>
#include <vector>

namespace {

TEST(Vst2MidiHelperContractTest, CopiesBorrowedShortAndSysExPayloadsImmediately) {
    VstHostMidiBuffer buffer{};
    vst_host_midi_clear(&buffer);
    uint8_t shortBytes[4] = {0x90, 60, 100, 0};
    ASSERT_EQ(vst_host_midi_append_vst_event(&buffer, 7, 1, 3, shortBytes), 1);
    const uint8_t sysex[] = {0xf0, 1, 2, 3, 0xf7};
    ASSERT_EQ(vst_host_midi_append_vst_sysex(&buffer, 19, 6, 0,
                                             sizeof(sysex), sysex), 1);
    shortBytes[0] = 0xff;
    EXPECT_EQ(buffer.count, 2u);
    EXPECT_EQ(buffer.events[0].frame_offset, 7u);
    EXPECT_EQ(buffer.payload[buffer.events[0].payload_offset], 0x90u);
    EXPECT_EQ(buffer.events[1].frame_offset, 19u);
    EXPECT_EQ(buffer.events[1].payload_size, sizeof(sysex));
    EXPECT_EQ(buffer.payload[buffer.events[1].payload_offset], 0xf0u);
    EXPECT_EQ(buffer.payload[buffer.events[1].payload_offset + sizeof(sysex) - 1], 0xf7u);
}

TEST(Vst2MidiHelperContractTest, RejectsMalformedSysExAndPreservesCurrentNextCopies) {
    VstHostMidiBuffer current{};
    vst_host_midi_clear(&current);
    const uint8_t malformed[] = {0xf0, 1, 2};
    EXPECT_EQ(vst_host_midi_append_vst_sysex(&current, 0, 6, 0,
                                             sizeof(malformed), malformed), 0);
    EXPECT_EQ(current.dropped, 1u);
    const uint8_t note[] = {0x90, 64, 110};
    ASSERT_EQ(vst_host_midi_append(&current, 3, note, sizeof(note)), 1);
    VstHostMidiBuffer next{};
    EXPECT_EQ(vst_host_midi_copy(&next, &current), 1);
    current.payload[current.events[0].payload_offset + 1] = 1;
    EXPECT_EQ(next.payload[next.events[0].payload_offset + 1], 64u);
}

TEST(Vst2MidiHelperContractTest, DistinguishesNegativeDeltaFromSystemAndRealtimeMessages) {
    VstHostMidiBuffer buffer{};
    vst_host_midi_clear(&buffer);
    const uint8_t program[] = {0xc0, 9, 0, 0};
    EXPECT_EQ(vst_host_midi_append_vst_event(&buffer, -3, 1, 2, program), 0);
    EXPECT_EQ(buffer.dropped, 1u);
    const uint8_t system[] = {0xf1, 1, 0, 0};
    EXPECT_EQ(vst_host_midi_append_vst_event(&buffer, 2, 1, 2, system), 1);
    const uint8_t realtime[] = {0xf8, 0, 0, 0};
    EXPECT_EQ(vst_host_midi_append_vst_event(&buffer, 3, 1, 1, realtime), 1);
    EXPECT_EQ(buffer.count, 2u);
    EXPECT_EQ(buffer.events[0].frame_offset, 2u);
    EXPECT_EQ(buffer.events[0].payload_size, 2u);
    EXPECT_EQ(buffer.events[1].frame_offset, 3u);
    EXPECT_EQ(buffer.events[1].payload_size, 1u);
}

TEST(Vst2MidiHelperContractTest, AuthoritySelectsCurrentOrNextIncludingIntentionalEmpty) {
    VstHostMidiBuffer current{};
    VstHostMidiBuffer next{};
    vst_host_midi_clear(&current);
    vst_host_midi_clear(&next);
    const uint8_t note[] = {0x90, 60, 100};
    ASSERT_EQ(vst_host_midi_append(&current, 0, note, sizeof(note)), 1);
    EXPECT_EQ(vst_host_midi_select(&current, &next, 0), &current);
    EXPECT_EQ(vst_host_midi_select(&current, &next, 1), &next);
    EXPECT_EQ(vst_host_midi_select(&current, &next, 1)->count, 0u);
}

TEST(Vst3MidiHelperContractTest, AcceptsChannelVoiceAndCompleteSysExOnly) {
    using namespace vstpoc::vst3;
    const uint8_t note[] = {0x90, 60, 100};
    const uint8_t program[] = {0xc0, 4};
    const uint8_t sysex[] = {0xf0, 0x7d, 1, 0xf7};
    const uint8_t incomplete[] = {0xf0, 0x7d, 1};
    EXPECT_TRUE(isSupportedInput(note, sizeof(note)));
    EXPECT_TRUE(isSupportedInput(program, sizeof(program)));
    EXPECT_TRUE(isSupportedInput(sysex, sizeof(sysex)));
    EXPECT_FALSE(isSupportedInput(incomplete, sizeof(incomplete)));
}

TEST(Vst3MidiHelperContractTest, AcceptsExactPayloadLimitAndRejectsWholeNextMessage) {
    using namespace vstpoc::vst3;
    std::vector<uint8_t> payload(kMaxPayload, 0x7d);
    payload.front() = 0xf0;
    payload.back() = 0xf7;
    MessageBuffer buffer;
    ASSERT_TRUE(buffer.append(0, payload.data(), payload.size()));
    const uint8_t note[] = {0x90, 1, 2};
    EXPECT_FALSE(buffer.append(1, note, sizeof(note)));
    EXPECT_EQ(buffer.size(), 1u);
    EXPECT_EQ(buffer.rejected(), 1u);
}

TEST(Vst3MidiHelperContractTest, StableMergePreservesSystemOrderAndRejectsOverflow) {
    using namespace vstpoc::vst3;
    MessageBuffer supported;
    const uint8_t note[] = {0x90, 60, 100};
    const uint8_t sysex[] = {0xf0, 1, 2, 0xf7};
    ASSERT_TRUE(supported.append(4, note, sizeof(note)));
    ASSERT_TRUE(supported.append(8, sysex, sizeof(sysex)));
    MessageBuffer systems;
    const uint8_t clock[] = {0xf8};
    ASSERT_TRUE(systems.append(4, clock, sizeof(clock)));
    MessageBuffer merged;
    supported.mergeInto(merged, systems);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged.at(0).bytes[0], 0x90u);
    EXPECT_EQ(merged.at(1).bytes[0], 0xf8u);
    EXPECT_EQ(merged.at(2).bytes[0], 0xf0u);

    MessageBuffer full;
    for (std::size_t i = 0; i < kMaxEvents; ++i)
        ASSERT_TRUE(full.append(static_cast<uint32_t>(i), note, sizeof(note)));
    EXPECT_FALSE(full.append(999, sysex, sizeof(sysex)));
    EXPECT_EQ(full.rejected(), 1u);
}

}  // namespace
