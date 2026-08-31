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
    first.name = "Барабаны";
    first.colorArgb = 0xff123456u;

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
    second.name = "Guitar";
    second.colorArgb = 0xffc0ffeeu;


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

void expectClipSlot(const RackGraph::State::ClipSlot& actual,
                    const RackGraph::State::ClipSlot& expected) {
    EXPECT_EQ(actual.slot, expected.slot);
    EXPECT_EQ(actual.wav, expected.wav);
    EXPECT_EQ(actual.midi, expected.midi);
    EXPECT_EQ(actual.assetId, expected.assetId);
    EXPECT_EQ(actual.midiAssetId, expected.midiAssetId);
    EXPECT_EQ(actual.displayName, expected.displayName);
    EXPECT_EQ(actual.sourceBpm, expected.sourceBpm);
    EXPECT_EQ(actual.tempoMode, expected.tempoMode);
    EXPECT_EQ(actual.looping, expected.looping);
    EXPECT_EQ(actual.loopLengthBars, expected.loopLengthBars);
    EXPECT_EQ(actual.defaultLoopLengthBars, expected.defaultLoopLengthBars);
    EXPECT_EQ(actual.loopStartQuarterNotes, expected.loopStartQuarterNotes);
    EXPECT_EQ(actual.loopLengthQuarterNotes, expected.loopLengthQuarterNotes);
    EXPECT_EQ(actual.enterOnPunch, expected.enterOnPunch);
    EXPECT_EQ(actual.launchQuantization, expected.launchQuantization);
}

void expectPlugin(const PluginState& actual, const PluginState& expected) {
    EXPECT_EQ(actual.format, expected.format);
    EXPECT_EQ(actual.pluginUri, expected.pluginUri);
    EXPECT_EQ(actual.manualLatencyFrames, expected.manualLatencyFrames);
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

        EXPECT_EQ(a.name, e.name);
        EXPECT_EQ(a.colorArgb, e.colorArgb);
        EXPECT_EQ(a.selectedSlot, e.selectedSlot);
        EXPECT_EQ(a.defaultLoopLengthBars, e.defaultLoopLengthBars);

        ASSERT_EQ(a.clipSlots.size(), e.clipSlots.size());
        for (size_t j = 0; j < e.clipSlots.size(); ++j)
            expectClipSlot(a.clipSlots[j], e.clipSlots[j]);
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
constexpr size_t kV4TrackNameLengthOffset = 52;
constexpr size_t kV4TrackNameDataOffset = 56;

std::string supplementaryName(size_t count) {
    std::string result;
    result.reserve(count * 6);
    // U+1F600 encoded as its UTF-16 surrogate pair in modified UTF-8.
    for (size_t i = 0; i < count; ++i)
        result.append("\xed\xa0\xbd\xed\xb8\x80", 6);
    return result;
}

std::vector<uint8_t> encodeSingleTrackName(const std::string& name) {
    RackGraph::State state;
    RackGraph::State::Track track;
    track.id = 23;
    track.name = name;
    state.tracks.push_back(std::move(track));

    std::string error;
    std::vector<uint8_t> encoded = RackStateCodec::encode(state, &error);
    EXPECT_FALSE(encoded.empty()) << error;
    return encoded;
}

void setTrackNameBytes(std::vector<uint8_t>& encoded, size_t offset,
                       std::initializer_list<uint8_t> bytes) {
    const size_t nameLength =
        getU32(encoded, kV4TrackNameLengthOffset);
    ASSERT_LE(offset + bytes.size(), nameLength);
    size_t index = kV4TrackNameDataOffset + offset;
    for (uint8_t byte : bytes)
        encoded[index++] = byte;
    refreshCrc(encoded);
}

void appendTrackNameBytes(std::vector<uint8_t>& encoded,
                          std::initializer_list<uint8_t> bytes) {
    const size_t nameLength =
        getU32(encoded, kV4TrackNameLengthOffset);
    const size_t insertOffset = kV4TrackNameDataOffset + nameLength;
    encoded.insert(encoded.begin() + insertOffset, bytes.begin(), bytes.end());
    putU32(encoded, kV4TrackNameLengthOffset,
           static_cast<uint32_t>(nameLength + bytes.size()));
    refreshCrc(encoded);
}

void expectInvalidTrackNamePayload(const std::vector<uint8_t>& encoded) {
    RackGraph::State existing = fixtureState();
    existing.tracks[0].id = 123;
    const RackGraph::State before = existing;
    std::string error;
    EXPECT_FALSE(RackStateCodec::decode(
        encoded.data(), encoded.size(), existing, error));
    EXPECT_EQ(error, "invalid-track-name");
    expectState(existing, before);
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
// Strip the v4 per-track metadata while retaining the v3 track configuration,
// clip records, and version-3 chain/global wire format. This keeps compatibility
// tests based on payloads emitted by the current encoder rather than hand-built
// bytes.
std::vector<uint8_t> makeV3Payload(const std::vector<uint8_t>& v4) {
    if (v4.size() < 12 || getU32(v4, 4) != 4u)
        return {};
    const size_t limit = v4.size() - 4;
    size_t offset = 8;
    uint32_t trackCount = 0;
    if (!readU32(v4, limit, offset, trackCount))
        return {};

    std::vector<uint8_t> v3(v4.begin(), v4.begin() + 12);
    putU32(v3, 4, 3);
    const auto copyRange = [&v3, &v4](size_t begin, size_t end) {
        v3.insert(v3.end(), v4.begin() + begin, v4.begin() + end);
    };
    for (uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        const size_t trackStart = offset;
        if (!skipBytes(v4, limit, offset, 28u) ||
            !skipBytes(v4, limit, offset, 12u))
            return {};
        copyRange(trackStart, offset);
        if (!skipWireString(v4, limit, offset) ||
            !skipBytes(v4, limit, offset, 4u))
            return {};
        const size_t clipStart = offset;
        uint32_t clipCount = 0;
        if (!readU32(v4, limit, offset, clipCount))
            return {};
        for (uint32_t clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
            if (!skipBytes(v4, limit, offset, 6u) ||
                !skipWireString(v4, limit, offset) ||
                !skipWireString(v4, limit, offset) ||
                !skipWireString(v4, limit, offset) ||
                !skipBytes(v4, limit, offset, 47u))
                return {};
        }
        copyRange(clipStart, offset);
        const size_t chainStart = offset;
        std::optional<size_t> unusedPropertyOffset;
        if (!skipChain(v4, limit, offset, 4u, unusedPropertyOffset))
            return {};
        copyRange(chainStart, offset);
    }

    const size_t masterStart = offset;
    std::optional<size_t> unusedPropertyOffset;
    if (!skipChain(v4, limit, offset, 4u, unusedPropertyOffset))
        return {};
    copyRange(masterStart, offset);
    const size_t globalStart = offset;
    if (!skipBytes(v4, limit, offset, 33u) || offset != limit)
        return {};
    copyRange(globalStart, limit);
    v3.resize(v3.size() + 4u);
    refreshCrc(v3);
    return v3;
}



// Strip the v3 track configuration and clip records while retaining the
// version-2 chain/global wire format. This keeps compatibility tests based on
// payloads emitted by the current encoder rather than hand-built bytes.
std::vector<uint8_t> makeV2Payload(const std::vector<uint8_t>& v3) {
    if (v3.size() < 12 || getU32(v3, 4) != 3u)
        return {};
    const size_t limit = v3.size() - 4;
    size_t offset = 8;
    uint32_t trackCount = 0;
    if (!readU32(v3, limit, offset, trackCount))
        return {};

    std::vector<uint8_t> v2(v3.begin(), v3.begin() + 12);
    putU32(v2, 4, 2);
    const auto copyRange = [&v2, &v3](size_t begin, size_t end) {
        v2.insert(v2.end(), v3.begin() + begin, v3.begin() + end);
    };
    for (uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        const size_t trackStart = offset;
        if (!skipBytes(v3, limit, offset, 28u) ||
            !skipBytes(v3, limit, offset, 12u))
            return {};
        uint32_t clipCount = 0;
        if (!readU32(v3, limit, offset, clipCount))
            return {};
        for (uint32_t clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
            if (!skipBytes(v3, limit, offset, 6u) ||
                !skipWireString(v3, limit, offset) ||
                !skipWireString(v3, limit, offset) ||
                !skipWireString(v3, limit, offset) ||
                !skipBytes(v3, limit, offset, 47u))
                return {};
        }
        copyRange(trackStart, trackStart + 28u);
        const size_t chainStart = offset;
        std::optional<size_t> unusedPropertyOffset;
        if (!skipChain(v3, limit, offset, 3u, unusedPropertyOffset))
            return {};
        copyRange(chainStart, offset);
    }

    const size_t masterStart = offset;
    std::optional<size_t> unusedPropertyOffset;
    if (!skipChain(v3, limit, offset, 3u, unusedPropertyOffset))
        return {};
    copyRange(masterStart, offset);
    // BPM, transport state, transport frame, sample position, and musical
    // quarter notes are identical in v2 and v3.
    const size_t globalStart = offset;
    if (!skipBytes(v3, limit, offset, 33u) || offset != limit)
        return {};
    copyRange(globalStart, limit);
    v2.resize(v2.size() + 4u);
    refreshCrc(v2);
    return v2;
}
TEST(RackStateCodecTest,
     V4DecodeAcceptsMaximumNameOf48SupplementaryUnicodeScalars) {
    const std::string expectedName = supplementaryName(48);
    const std::vector<uint8_t> encoded = encodeSingleTrackName(expectedName);
    ASSERT_FALSE(encoded.empty());
    ASSERT_EQ(getU32(encoded, kV4TrackNameLengthOffset), 288u);

    RackGraph::State decoded;
    std::string error;
    ASSERT_TRUE(RackStateCodec::decode(
        encoded.data(), encoded.size(), decoded, error)) << error;
    ASSERT_EQ(decoded.tracks.size(), 1u);
    EXPECT_EQ(decoded.tracks.front().name, expectedName);
}

TEST(RackStateCodecTest,
     V4DecodeRejects49UnicodeScalarsAtomically) {
    std::vector<uint8_t> encoded =
        encodeSingleTrackName(std::string(48, 'a'));
    ASSERT_FALSE(encoded.empty());
    appendTrackNameBytes(encoded, {'b'});
    ASSERT_EQ(getU32(encoded, kV4TrackNameLengthOffset), 49u);

    expectInvalidTrackNamePayload(encoded);
}

TEST(RackStateCodecTest, V4DecodeRejectsNamesOver288EncodedBytesAtomically) {
    std::vector<uint8_t> encoded =
        encodeSingleTrackName(supplementaryName(48));
    ASSERT_FALSE(encoded.empty());
    appendTrackNameBytes(encoded, {'b'});
    ASSERT_EQ(getU32(encoded, kV4TrackNameLengthOffset), 289u);

    expectInvalidTrackNamePayload(encoded);
}

TEST(RackStateCodecTest,
     V4DecodeRejectsMalformedModifiedUtf8TrackNamesAtomically) {
    struct Corruption {
        const char* name;
        std::function<void(std::vector<uint8_t>&)> mutate;
    };
    const std::vector<Corruption> corruptions = {
        {"raw-nul", [](auto& data) { setTrackNameBytes(data, 4, {0x00}); }},
        {"encoded-nul", [](auto& data) {
             setTrackNameBytes(data, 3, {0xc0, 0x80});
         }},
        {"unpaired-high-surrogate", [](auto& data) {
             setTrackNameBytes(data, 3, {0xed, 0xa0, 0x80});
         }},
        {"unpaired-low-surrogate", [](auto& data) {
             setTrackNameBytes(data, 3, {0xed, 0xb8, 0x80});
         }},
        {"standard-four-byte-utf8", [](auto& data) {
             setTrackNameBytes(data, 1, {0xf0, 0x9f, 0x98, 0x80});
         }},
        {"invalid-continuation", [](auto& data) {
             setTrackNameBytes(data, 1, {0xc2, 0x41});
         }},
        {"overlong-two-byte", [](auto& data) {
             setTrackNameBytes(data, 1, {0xc1, 0x81});
         }},
        {"overlong-three-byte", [](auto& data) {
             setTrackNameBytes(data, 1, {0xe0, 0x80, 0x80});
         }},
        {"truncated-sequence", [](auto& data) {
             setTrackNameBytes(data, 5, {0xe2});
         }},
    };

    const std::vector<uint8_t> encoded = encodeSingleTrackName("valid!");
    ASSERT_FALSE(encoded.empty());
    ASSERT_EQ(getU32(encoded, kV4TrackNameLengthOffset), 6u);

    for (const Corruption& corruption : corruptions) {
        SCOPED_TRACE(corruption.name);
        std::vector<uint8_t> damaged = encoded;
        corruption.mutate(damaged);
        expectInvalidTrackNamePayload(damaged);
    }
}


TEST(RackStateCodecTest, RoundTripPreservesRoutingPluginsParametersBinaryPropertiesAndTrackMetadata) {
    const RackGraph::State expected = fixtureState();
    std::string error;
    const std::vector<uint8_t> encoded = RackStateCodec::encode(expected, &error);
    ASSERT_FALSE(encoded.empty()) << error;
    EXPECT_EQ(getU32(encoded, 4), 4u);


    RackGraph::State decoded;
    ASSERT_TRUE(RackStateCodec::decode(encoded.data(), encoded.size(), decoded, error)) << error;
    expectState(decoded, expected);
}

TEST(RackStateCodecTest,
     V4RoundTripPreservesTrackMetadataClipConfigurationAssetIdsAndCompletePluginChains) {

    RackGraph::State expected = fixtureState();
    auto& track = expected.tracks.front();
    track.selectedSlot = 9;
    track.defaultLoopLengthBars = 3.5;

    RackGraph::State::ClipSlot wav;
    wav.slot = 9;
    wav.wav = true;
    wav.assetId = "opaque-wav-asset-id";
    wav.displayName = "Loop (WAV)";
    wav.sourceBpm = 98.25;
    wav.tempoMode = 2;
    wav.looping = true;
    wav.loopLengthBars = 7.0;
    wav.defaultLoopLengthBars = 2.0;
    wav.loopStartQuarterNotes = 1.25;
    wav.loopLengthQuarterNotes = 28.0;
    wav.enterOnPunch = true;
    wav.launchQuantization = LaunchQuantization::Eighth;

    RackGraph::State::ClipSlot midi;
    midi.slot = 12;
    midi.midi = true;
    midi.midiAssetId = "opaque-midi-asset-id";
    midi.displayName = "Bass MIDI";
    midi.sourceBpm = 143.75;
    midi.tempoMode = 1;
    midi.looping = false;
    midi.loopLengthBars = 5.5;
    midi.defaultLoopLengthBars = 1.5;
    midi.loopStartQuarterNotes = 2.0;
    midi.loopLengthQuarterNotes = 22.0;
    midi.enterOnPunch = false;
    midi.launchQuantization = LaunchQuantization::None;
    track.clipSlots = {wav, midi};

    std::string error;
    const std::vector<uint8_t> encoded = RackStateCodec::encode(expected, &error);
    ASSERT_FALSE(encoded.empty()) << error;
    EXPECT_EQ(getU32(encoded, 4), 4u);


    RackGraph::State decoded;
    ASSERT_TRUE(RackStateCodec::decode(encoded.data(), encoded.size(), decoded, error)) << error;
    expectState(decoded, expected);
}

TEST(RackStateCodecTest, V3PayloadDefaultsTrackMetadataAndV1DefaultsManualLatencyOverridesToZero) {
    RackGraph::State expected;
    RackGraph::State::Track track;
    track.id = 23;
    track.name = "Legacy";
    track.colorArgb = 0xffabcdefu;
    track.selectedSlot = 4;
    track.defaultLoopLengthBars = 2.25;
    RackGraph::State::ClipSlot clip;
    clip.slot = 4;
    clip.wav = true;
    clip.assetId = "legacy-clip";
    track.clipSlots.push_back(clip);
    track.chain.plugins.push_back(plugin("JSFX", "JSFX:legacy", {}));
    expected.tracks.push_back(track);

    std::string error;
    const std::vector<uint8_t> v4 = RackStateCodec::encode(expected, &error);
    ASSERT_FALSE(v4.empty()) << error;
    const std::vector<uint8_t> v3 = makeV3Payload(v4);
    ASSERT_FALSE(v3.empty());
    EXPECT_EQ(getU32(v3, 4), 3u);

    RackGraph::State decodedV3;
    ASSERT_TRUE(RackStateCodec::decode(
        v3.data(), v3.size(), decodedV3, error)) << error;
    ASSERT_EQ(decodedV3.tracks.size(), 1u);
    RackGraph::State expectedV3 = expected;
    expectedV3.tracks.front().name.clear();
    expectedV3.tracks.front().colorArgb = 0;
    expectState(decodedV3, expectedV3);

    EXPECT_EQ(decodedV3.tracks.front().name, "");
    EXPECT_EQ(decodedV3.tracks.front().colorArgb, 0u);

    const std::vector<uint8_t> encoded = makeV2Payload(v3);
    ASSERT_FALSE(encoded.empty());
    EXPECT_EQ(getU32(encoded, 4), 2u);
    const size_t manualLatencyOffset =
        40 + 4 + 4 + std::string("JSFX").size() +
        4 + std::string("JSFX:legacy").size() + 4 + 4;
    EXPECT_EQ(getU32(encoded, manualLatencyOffset), 0u);
    const std::vector<uint8_t> v1 = makeV1Payload(encoded, manualLatencyOffset);
    RackGraph::State decoded;
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

    const std::vector<uint8_t> v4 = RackStateCodec::encode(expected);
    ASSERT_FALSE(v4.empty());
    const std::vector<uint8_t> v3 = makeV3Payload(v4);
    ASSERT_FALSE(v3.empty());
    std::vector<uint8_t> encoded = makeV2Payload(v3);

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
    const std::vector<uint8_t> v4 = RackStateCodec::encode(expected, &error);
    ASSERT_FALSE(v4.empty()) << error;
    const std::vector<uint8_t> v3 = makeV3Payload(v4);
    ASSERT_FALSE(v3.empty());
    const std::vector<uint8_t> encoded = makeV2Payload(v3);
    ASSERT_FALSE(encoded.empty());
    ASSERT_EQ(getU32(encoded, 4), 2u);

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
    const std::vector<uint8_t> v4 = RackStateCodec::encode(expected);
    ASSERT_FALSE(v4.empty());
    const std::vector<uint8_t> v3 = makeV3Payload(v4);
    ASSERT_FALSE(v3.empty());
    const std::vector<uint8_t> encoded = makeV2Payload(v3);

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

TEST(RackStateCodecTest, DeviceChainDecodeRejectsProjectWithClipData) {
    RackGraph::State project;
    RackGraph::State::Track track;
    track.id = 314;
    track.chain.plugins.push_back(plugin("LV2", "http://example.test/clip", {{2, 0.4f}}));
    RackGraph::State::ClipSlot clip;
    clip.slot = 4;
    clip.wav = true;
    clip.assetId = "opaque-project-wav";
    clip.displayName = "Project clip";
    track.clipSlots.push_back(clip);
    project.tracks.push_back(track);

    std::string error;
    const std::vector<uint8_t> encoded = RackStateCodec::encode(project, &error);
    ASSERT_FALSE(encoded.empty()) << error;

    RackPathId pathId = 99;
    PluginChain::ChainState existing;
    existing.plugins.push_back(plugin("JSFX", "http://example.test/old", {{1, 0.2f}}));
    const RackPathId beforePath = pathId;
    const PluginChain::ChainState beforeChain = existing;
    EXPECT_FALSE(RackStateCodec::decodeDeviceChain(
        encoded.data(), encoded.size(), pathId, existing, error));
    EXPECT_EQ(error, "not-device-chain");
    EXPECT_EQ(pathId, beforePath);
    ASSERT_EQ(existing.plugins.size(), beforeChain.plugins.size());
    expectPlugin(existing.plugins.front(), beforeChain.plugins.front());
}

TEST(RackStateCodecTest,
     MalformedDeviceChainPayloadPreservesExistingPathAndChain) {
    PluginChain::ChainState source;
    source.plugins.push_back(plugin("LV2", "http://example.test/new", {{1, 0.5f}}));
    std::vector<uint8_t> damaged = RackStateCodec::encodeDeviceChain(42, source);
    ASSERT_FALSE(damaged.empty());

    // The v4 track envelope is 64 bytes before the scoped chain's plugin
    // count. Keep the CRC valid so this exercises bounded decoding, not only
    // checksum rejection.
    putU32(damaged, 64, std::numeric_limits<uint32_t>::max());
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
