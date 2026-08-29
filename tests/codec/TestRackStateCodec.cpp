#include "RackStateCodec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
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
                   std::initializer_list<StateProperty> properties = {},
                   uint32_t manualLatencyFrames = 0) {
    PluginState result;
    result.format = std::move(format);
    result.pluginUri = std::move(uri);
    result.controlPortValues.assign(controls.begin(), controls.end());
    result.properties.assign(properties.begin(), properties.end());
    result.manualLatencyFrames = manualLatencyFrames;
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
          "urn:type:blob", 0x10203040u}},
        37));
    first.chain.plugins.push_back(plugin(
        "LV2", "http://example.test/beta", {{0, 1.0f}, {9, 0.0f}}, {}, 0));

    RackGraph::State::Track second;
    second.id = 42;
    second.volume = 0.875f;
    second.inputArmed = false;
    second.inputArmLocked = true;
    second.inputSource.kind = TrackInputSource::Kind::HardwareMono;
    second.inputSource.tap = TrackInputTap::PreFader;
    second.inputSource.firstChannel = 11;
    second.chain.plugins.push_back(plugin("VST3", "vst3:gamma", {{4, 0.33333334f}}, {}, 19));

    state.tracks = {first, second};
    state.master.plugins.push_back(plugin("JSFX", "JSFX:master", {{1, 0.75f}}, {}, 7));
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
    EXPECT_EQ(actual.manualLatencyFrames, expected.manualLatencyFrames);
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
std::vector<uint8_t> makeV1Payload(std::vector<uint8_t> v2,
                                   size_t manualLatencyOffset) {
    EXPECT_LE(manualLatencyOffset + 4, v2.size());
    v2.erase(v2.begin() + manualLatencyOffset,
             v2.begin() + manualLatencyOffset + 4);
    putU32(v2, 4, 1);
    refreshCrc(v2);
    return v2;
}

bool readU32(const std::vector<uint8_t>& data, size_t limit, size_t& offset,
             uint32_t& value) {
    if (offset > limit || limit - offset < 4)
        return false;
    value = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool skipBytes(const std::vector<uint8_t>& data, size_t limit, size_t& offset,
               size_t size) {
    if (offset > limit || size > limit - offset)
        return false;
    offset += size;
    return true;
}

bool skipWireString(const std::vector<uint8_t>& data, size_t limit,
                    size_t& offset) {
    uint32_t size = 0;
    return readU32(data, limit, offset, size) &&
           skipBytes(data, limit, offset, size);
}

bool skipChain(const std::vector<uint8_t>& data, size_t limit, size_t& offset,
               uint32_t version, std::optional<size_t>& firstPropertyOffset) {
    uint32_t pluginCount = 0;
    if (!readU32(data, limit, offset, pluginCount))
        return false;
    const size_t minimumPluginBytes = version >= 2 ? 20u : 16u;
    if (pluginCount > (limit - offset) / minimumPluginBytes)
        return false;

    for (uint32_t pluginIndex = 0; pluginIndex < pluginCount; ++pluginIndex) {
        if (!skipWireString(data, limit, offset) ||
            !skipWireString(data, limit, offset))
            return false;

        uint32_t controlCount = 0;
        if (!readU32(data, limit, offset, controlCount) ||
            controlCount > (limit - offset) / 8u ||
            !skipBytes(data, limit, offset,
                       static_cast<size_t>(controlCount) * 8u))
            return false;

        uint32_t propertyCount = 0;
        if (!readU32(data, limit, offset, propertyCount) ||
            propertyCount > (limit - offset) / 16u)
            return false;
        for (uint32_t propertyIndex = 0; propertyIndex < propertyCount;
             ++propertyIndex) {
            if (!skipWireString(data, limit, offset) ||
                !skipWireString(data, limit, offset) ||
                !skipBytes(data, limit, offset, 4u))
                return false;
            if (pluginIndex == 0 && propertyIndex == 0)
                firstPropertyOffset = offset;

            uint32_t valueSize = 0;
            if (!readU32(data, limit, offset, valueSize) ||
                !skipBytes(data, limit, offset, valueSize))
                return false;
        }
        if (version >= 2 && !skipBytes(data, limit, offset, 4u))
            return false;
    }
    return true;
}

// Locate the first property's byte-count field through the versioned wire
// format, checking every read against the payload boundary.
std::optional<size_t> firstPropertyLengthOffset(
    const std::vector<uint8_t>& data) {
    if (data.size() < 12)
        return std::nullopt;
    const size_t limit = data.size() - 4; // Exclude the CRC.
    size_t offset = 4;
    uint32_t version = 0;
    if (!readU32(data, limit, offset, version) || (version != 1 && version != 2))
        return std::nullopt;

    offset = 8;
    uint32_t trackCount = 0;
    if (!readU32(data, limit, offset, trackCount) ||
        trackCount > (limit - offset) / 28u)
        return std::nullopt;

    std::optional<size_t> propertyOffset;
    for (uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        if (!skipBytes(data, limit, offset, 28u) ||
            !skipChain(data, limit, offset, version, propertyOffset))
            return std::nullopt;
    }
    if (!skipChain(data, limit, offset, version, propertyOffset))
        return std::nullopt;
    return propertyOffset;
}

TEST(RackStateCodecTest, RoundTripPreservesRoutingPluginsParametersAndBinaryProperties) {
    const RackGraph::State expected = fixtureState();
    std::string error;
    const std::vector<uint8_t> encoded = RackStateCodec::encode(expected, &error);
    ASSERT_FALSE(encoded.empty()) << error;
    EXPECT_EQ(getU32(encoded, 4), 2u);

    RackGraph::State decoded;
    ASSERT_TRUE(RackStateCodec::decode(encoded.data(), encoded.size(), decoded, error)) << error;
    expectState(decoded, expected);
}

TEST(RackStateCodecTest, V1PayloadDefaultsManualLatencyOverridesToZero) {
    RackGraph::State expected;
    RackGraph::State::Track track;
    track.id = 23;
    track.chain.plugins.push_back(plugin("JSFX", "JSFX:legacy", {}));
    expected.tracks.push_back(track);

    const std::vector<uint8_t> encoded = RackStateCodec::encode(expected);
    ASSERT_FALSE(encoded.empty());
    const size_t manualLatencyOffset =
        40 + 4 + 4 + std::string("JSFX").size() +
        4 + std::string("JSFX:legacy").size() + 4 + 4;
    EXPECT_EQ(getU32(encoded, manualLatencyOffset), 0u);
    const std::vector<uint8_t> v1 = makeV1Payload(encoded, manualLatencyOffset);

    RackGraph::State decoded;
    std::string error;
    ASSERT_TRUE(RackStateCodec::decode(v1.data(), v1.size(), decoded, error)) << error;
    ASSERT_EQ(decoded.tracks.size(), 1u);
    ASSERT_EQ(decoded.tracks.front().chain.plugins.size(), 1u);
    EXPECT_EQ(decoded.tracks.front().chain.plugins.front().manualLatencyFrames, 0u);
}
TEST(RackStateCodecTest, V2PayloadRejectsOversizedManualLatencyOverrideAtomically) {
    RackGraph::State expected;
    RackGraph::State::Track track;
    track.id = 23;
    track.chain.plugins.push_back(plugin("JSFX", "JSFX:oversized", {}));
    expected.tracks.push_back(track);

    std::vector<uint8_t> encoded = RackStateCodec::encode(expected);
    ASSERT_FALSE(encoded.empty());
    ASSERT_EQ(getU32(encoded, 4), 2u);

    const size_t manualLatencyOffset =
        40 + 4 + 4 + std::string("JSFX").size() +
        4 + std::string("JSFX:oversized").size() + 4 + 4;
    putU32(encoded, manualLatencyOffset,
           PluginChain::kMaxSupportedPdcFrames + 1);
    refreshCrc(encoded);

    RackGraph::State existing = fixtureState();
    existing.tracks[0].id = 999;
    const RackGraph::State before = existing;
    std::string error;
    EXPECT_FALSE(RackStateCodec::decode(
        encoded.data(), encoded.size(), existing, error));
    EXPECT_EQ(error, "invalid-track");
    expectState(existing, before);
}

TEST(RackStateCodecTest, V2PayloadAcceptsMaximumManualLatencyOverride) {
    RackGraph::State expected;
    RackGraph::State::Track track;
    track.id = 23;
    track.chain.plugins.push_back(plugin(
        "JSFX", "JSFX:maximum",
        {}, {}, PluginChain::kMaxSupportedPdcFrames));
    expected.tracks.push_back(track);

    std::string error;
    const std::vector<uint8_t> encoded =
        RackStateCodec::encode(expected, &error);
    ASSERT_FALSE(encoded.empty()) << error;

    RackGraph::State decoded;
    ASSERT_TRUE(RackStateCodec::decode(
        encoded.data(), encoded.size(), decoded, error)) << error;
    ASSERT_EQ(decoded.tracks.size(), 1u);
    ASSERT_EQ(decoded.tracks.front().chain.plugins.size(), 1u);
    EXPECT_EQ(decoded.tracks.front().chain.plugins.front().manualLatencyFrames,
              PluginChain::kMaxSupportedPdcFrames);
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
            const auto propertyOffset = firstPropertyLengthOffset(data);
            ASSERT_TRUE(propertyOffset.has_value());
            if (!propertyOffset)
                return;
            putU32(data, *propertyOffset, (8u << 20) + 1u);
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

TEST(RackStateCodecTest, DeviceChainEnvelopeRoundTripPreservesPathAndPlugins) {
    PluginChain::ChainState expected;
    expected.plugins.push_back(plugin(
        "LV2", "http://example.test/scoped",
        {{3, 0.625f}},
        {{"urn:state:blob", {0x00, 0xff, 0x01}, "urn:type:blob", 7u}},
        23));

    std::string error;
    const std::vector<uint8_t> encoded =
        RackStateCodec::encodeDeviceChain(42, expected, &error);
    ASSERT_FALSE(encoded.empty()) << error;

    RackPathId pathId = 999;
    PluginChain::ChainState decoded;
    ASSERT_TRUE(RackStateCodec::decodeDeviceChain(
        encoded.data(), encoded.size(), pathId, decoded, error)) << error;
    EXPECT_EQ(pathId, 42u);
    ASSERT_EQ(decoded.plugins.size(), 1u);
    expectPlugin(decoded.plugins.front(), expected.plugins.front());
}

TEST(RackStateCodecTest,
     MalformedDeviceChainPayloadPreservesExistingPathAndChain) {
    PluginChain::ChainState source;
    source.plugins.push_back(plugin("LV2", "http://example.test/new", {{1, 0.5f}}));
    std::vector<uint8_t> damaged = RackStateCodec::encodeDeviceChain(42, source);
    ASSERT_FALSE(damaged.empty());

    // The scoped chain begins after the fixed 40-byte envelope prefix. Keep
    // the CRC valid so this exercises bounded decoding, not only checksum
    // rejection.
    putU32(damaged, 40, std::numeric_limits<uint32_t>::max());
    refreshCrc(damaged);

    PluginChain::ChainState existing;
    existing.plugins.push_back(plugin("LV2", "http://example.test/old", {{1, 0.25f}}));
    RackPathId pathId = 7;
    const RackPathId beforePath = pathId;
    const PluginChain::ChainState beforeChain = existing;
    std::string error;

    EXPECT_FALSE(RackStateCodec::decodeDeviceChain(
        damaged.data(), damaged.size(), pathId, existing, error));
    EXPECT_EQ(error, "invalid-track");
    EXPECT_EQ(pathId, beforePath);
    ASSERT_EQ(existing.plugins.size(), beforeChain.plugins.size());
    expectPlugin(existing.plugins.front(), beforeChain.plugins.front());
}

} // namespace
} // namespace guitarrackcraft
