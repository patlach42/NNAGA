#include "RackStateCodec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>
namespace guitarrackcraft {
namespace {

PluginState plugin(std::string format, std::string uri,
                   std::initializer_list<std::pair<uint32_t, float>> controls,
                   std::initializer_list<StateProperty> properties = {}) {
    PluginState result;
    result.format = std::move(format);
    result.pluginUri = std::move(uri);
    result.controlPortValues.assign(controls.begin(), controls.end());
    result.properties.assign(properties.begin(), properties.end());
    return result;
}

RackGraph::State fixtureState() {
    RackGraph::State state;

    RackGraph::State::Track first;
    first.id = 0x0102030405060708ULL;
    first.volume = -0.375f;
    first.inputArmed = true;
    first.inputArmLocked = true;
    first.inputSource.kind = TrackInputSource::Kind::TrackOutput;
    first.inputSource.tap = TrackInputTap::PostFader;
    first.inputSource.firstChannel = -3;
    first.inputSource.trackId = 0x8877665544332211ULL;
    first.chain.plugins.push_back(plugin(
        "JSFX", "JSFX:alpha",
        {{7, 0.125f}, {2, -4.5f}},
        {{"urn:state:bytes", {0x00, 0x7f, 0x00, 0xff, 0x01},
          "urn:type:blob", 0x10203040u}}));
    first.chain.plugins.push_back(plugin(
        "LV2", "http://example.test/beta", {{0, 1.0f}, {9, 0.0f}}));

    RackGraph::State::Track second;
    second.id = 42;
    second.volume = 0.875f;
    second.inputArmed = false;
    second.inputArmLocked = true;
    second.inputSource.kind = TrackInputSource::Kind::HardwareMono;
    second.inputSource.tap = TrackInputTap::PreFader;
    second.inputSource.firstChannel = 11;
    second.inputSource.trackId = 0;
    second.chain.plugins.push_back(plugin("VST3", "vst3:gamma", {{4, 0.33333334f}}));

    state.tracks = {first, second};
    state.master.plugins.push_back(plugin("JSFX", "JSFX:master", {{1, 0.75f}}));
    state.beatsPerMinute = 137.25;
    state.transportPlaying = true;
    state.transportFrame = 0x1020304050607080ULL;
    state.samplePosition = 0xfedcba9876543210ULL;
    state.musicalQuarterNotes = 1234.5;
    return state;
}

void expectProperty(const StateProperty& actual, const StateProperty& expected) {
    EXPECT_EQ(actual.keyUri, expected.keyUri);
    EXPECT_EQ(actual.typeUri, expected.typeUri);
    EXPECT_EQ(actual.flags, expected.flags);
    EXPECT_EQ(actual.value, expected.value);
}

void expectPlugin(const PluginState& actual, const PluginState& expected) {
    EXPECT_EQ(actual.format, expected.format);
    EXPECT_EQ(actual.pluginUri, expected.pluginUri);
    ASSERT_EQ(actual.controlPortValues.size(), expected.controlPortValues.size());
    for (size_t i = 0; i < expected.controlPortValues.size(); ++i) {
        EXPECT_EQ(actual.controlPortValues[i].first, expected.controlPortValues[i].first);
        EXPECT_EQ(actual.controlPortValues[i].second, expected.controlPortValues[i].second);
    }
    ASSERT_EQ(actual.properties.size(), expected.properties.size());
    for (size_t i = 0; i < expected.properties.size(); ++i)
        expectProperty(actual.properties[i], expected.properties[i]);
}

void expectState(const RackGraph::State& actual, const RackGraph::State& expected) {
    ASSERT_EQ(actual.tracks.size(), expected.tracks.size());
    for (size_t i = 0; i < expected.tracks.size(); ++i) {
        const auto& a = actual.tracks[i];
        const auto& e = expected.tracks[i];
        EXPECT_EQ(a.id, e.id);
        EXPECT_EQ(a.volume, e.volume);
        EXPECT_EQ(a.inputArmed, e.inputArmed);
        EXPECT_EQ(a.inputArmLocked, e.inputArmLocked);
        EXPECT_EQ(a.inputSource.kind, e.inputSource.kind);
        EXPECT_EQ(a.inputSource.tap, e.inputSource.tap);
        EXPECT_EQ(a.inputSource.firstChannel, e.inputSource.firstChannel);
        EXPECT_EQ(a.inputSource.trackId, e.inputSource.trackId);
        ASSERT_EQ(a.chain.plugins.size(), e.chain.plugins.size());
        for (size_t j = 0; j < e.chain.plugins.size(); ++j)
            expectPlugin(a.chain.plugins[j], e.chain.plugins[j]);
    }
    ASSERT_EQ(actual.master.plugins.size(), expected.master.plugins.size());
    for (size_t i = 0; i < expected.master.plugins.size(); ++i)
        expectPlugin(actual.master.plugins[i], expected.master.plugins[i]);
    EXPECT_EQ(actual.beatsPerMinute, expected.beatsPerMinute);
    EXPECT_EQ(actual.transportPlaying, expected.transportPlaying);
    EXPECT_EQ(actual.transportFrame, expected.transportFrame);
    EXPECT_EQ(actual.samplePosition, expected.samplePosition);
    EXPECT_EQ(actual.musicalQuarterNotes, expected.musicalQuarterNotes);
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

void putU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    ASSERT_LE(offset + 4, data.size());
    for (size_t i = 0; i < 4; ++i)
        data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t getU32(const std::vector<uint8_t>& data, size_t offset) {
    EXPECT_LE(offset + 4, data.size());
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void refreshCrc(std::vector<uint8_t>& data) {
    ASSERT_GE(data.size(), 4u);
    const size_t offset = data.size() - 4;
    putU32(data, offset, crc32(data.data(), offset));
}

size_t skipString(const std::vector<uint8_t>& data, size_t offset) {
    return offset + 4 + getU32(data, offset);
}

// Locate the first property's byte-count field through the public wire format.
size_t firstPropertyLengthOffset(const std::vector<uint8_t>& data) {
    size_t offset = 40; // header/version/track count + one fixed-size track
    EXPECT_EQ(getU32(data, offset), 2u);
    offset += 4;
    offset = skipString(data, offset); // format
    offset = skipString(data, offset); // plugin URI
    const uint32_t controls = getU32(data, offset);
    EXPECT_EQ(controls, 2u);
    offset += 4 + static_cast<size_t>(controls) * 8;
    EXPECT_EQ(getU32(data, offset), 1u);
    offset += 4;
    offset = skipString(data, offset); // property key
    offset = skipString(data, offset); // property type
    offset += 4; // flags
    return offset;
}

TEST(RackStateCodecTest, RoundTripPreservesRoutingPluginsParametersAndBinaryProperties) {
    const RackGraph::State expected = fixtureState();
    std::string error;
    const std::vector<uint8_t> encoded = RackStateCodec::encode(expected, &error);
    ASSERT_FALSE(encoded.empty()) << error;

    RackGraph::State decoded;
    ASSERT_TRUE(RackStateCodec::decode(encoded.data(), encoded.size(), decoded, error)) << error;
    expectState(decoded, expected);
}

TEST(RackStateCodecTest, CrcFlipIsRejectedWithoutReplacingExistingState) {
    const RackGraph::State expected = fixtureState();
    std::vector<uint8_t> encoded = RackStateCodec::encode(expected);
    ASSERT_FALSE(encoded.empty());
    encoded[20] ^= 0x01;

    RackGraph::State existing = fixtureState();
    existing.tracks[0].id = 999;
    const RackGraph::State before = existing;
    std::string error;
    EXPECT_FALSE(RackStateCodec::decode(encoded.data(), encoded.size(), existing, error));
    EXPECT_EQ(error, "crc-mismatch");
    expectState(existing, before);
}

TEST(RackStateCodecTest, TruncatedPayloadIsRejectedWithoutPartialState) {
    const RackGraph::State expected = fixtureState();
    std::vector<uint8_t> encoded = RackStateCodec::encode(expected);
    ASSERT_FALSE(encoded.empty());
    encoded.resize(encoded.size() - 5); // remove one payload byte, retain a valid CRC slot
    encoded.insert(encoded.end(), 4, 0);
    refreshCrc(encoded);

    RackGraph::State existing = fixtureState();
    existing.tracks[0].id = 777;
    const RackGraph::State before = existing;
    std::string error;
    EXPECT_FALSE(RackStateCodec::decode(encoded.data(), encoded.size(), existing, error));
    EXPECT_FALSE(error.empty());
    expectState(existing, before);
}

TEST(RackStateCodecTest, InvalidCountsAndLengthsAreRejectedWithoutPartialState) {
    const RackGraph::State expected = fixtureState();
    const std::vector<uint8_t> encoded = RackStateCodec::encode(expected);
    ASSERT_FALSE(encoded.empty());

    struct Corruption {
        const char* name;
        std::function<void(std::vector<uint8_t>&)> mutate;
    };
    const std::vector<Corruption> corruptions = {
        {"track-count", [](auto& data) { putU32(data, 8, std::numeric_limits<uint32_t>::max()); refreshCrc(data); }},
        {"plugin-count", [](auto& data) { putU32(data, 40, std::numeric_limits<uint32_t>::max()); refreshCrc(data); }},
        {"string-length", [](auto& data) { putU32(data, 44, (1u << 20) + 1u); refreshCrc(data); }},
        {"property-length", [](auto& data) {
            putU32(data, firstPropertyLengthOffset(data), (8u << 20) + 1u);
            refreshCrc(data);
        }},
    };

    for (const Corruption& corruption : corruptions) {
        SCOPED_TRACE(corruption.name);
        std::vector<uint8_t> damaged = encoded;
        corruption.mutate(damaged);
        RackGraph::State existing = fixtureState();
        existing.tracks[0].id = 123;
        const RackGraph::State before = existing;
        std::string error;
        EXPECT_FALSE(RackStateCodec::decode(damaged.data(), damaged.size(), existing, error));
        EXPECT_FALSE(error.empty());
        expectState(existing, before);
    }
}

} // namespace
} // namespace guitarrackcraft
