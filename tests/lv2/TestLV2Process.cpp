#include <gtest/gtest.h>

#include "plugin/lv2/LV2Plugin.h"
#include "plugin/lv2/LV2PluginFactory.h"
#include "plugin/PluginChain.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>


#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <thread>
#include <vector>
namespace guitarrackcraft {

// LV2Plugin.cpp only needs this guard for its host-side destruction path.  The
// native test does not link Android/X11 UI code.
bool isCreatingPluginUI() {
    return false;
}

void setCreatingPluginUI(bool) {}

bool isCreatingPluginUIForDisplay(int) {
    return false;
}

}  // namespace guitarrackcraft

namespace {

class ScopedEnvironment final {
public:
    ScopedEnvironment(const char* name, const char* value)
        : name_(name), hadValue_(std::getenv(name) != nullptr) {
        if (hadValue_) oldValue_ = std::getenv(name);
        setenv(name, value, 1);
    }

    ~ScopedEnvironment() {
        if (hadValue_) {
            setenv(name_.c_str(), oldValue_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string oldValue_;
    bool hadValue_;
};


class LV2HostContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        environment_ = std::make_unique<ScopedEnvironment>("LV2_PATH", LV2_FIXTURE_DIR);
        world_ = lilv_world_new();
        ASSERT_NE(world_, nullptr);
        lilv_world_load_all(world_);
        uri_ = lilv_new_uri(world_, "https://guitarrackcraft.test/lv2/host-contract");
        ASSERT_NE(uri_, nullptr);
        generation_ = std::make_shared<const guitarrackcraft::LV2PluginGeneration>(world_);
        const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
        plugin_ = plugins ? lilv_plugins_get_by_uri(plugins, uri_) : nullptr;
        ASSERT_NE(plugin_, nullptr);
    }

    void TearDown() override {
        if (uri_) lilv_node_free(uri_);
        generation_.reset();
        world_ = nullptr;
    }

    std::unique_ptr<ScopedEnvironment> environment_;
    LilvWorld* world_ = nullptr;
    LilvNode* uri_ = nullptr;
    const LilvPlugin* plugin_ = nullptr;
    std::shared_ptr<const guitarrackcraft::LV2PluginGeneration> generation_;
};

}  // namespace
template <typename Processor>
guitarrackcraft::MidiOutputDisposition processNoMidi(
        Processor& processor,
        const float* const* inputs,
        float* const* outputs,
        uint32_t numFrames,
        const guitarrackcraft::AudioProcessContext& context) {
    guitarrackcraft::MidiBuffer inputMidi;
    guitarrackcraft::MidiBuffer outputMidi;
    return processor.process(inputs, outputs, numFrames, context, inputMidi,
                             outputMidi);
}
template <typename Processor>
void processChainNoMidi(
        Processor& processor,
        const float* const* inputs,
        float* const* outputs,
        uint32_t numFrames,
        const guitarrackcraft::AudioProcessContext& context) {
    guitarrackcraft::MidiBuffer inputMidi;
    guitarrackcraft::MidiBuffer outputMidi;
    processor.process(inputs, outputs, numFrames, context, inputMidi,
                      outputMidi);
}

TEST_F(LV2HostContractTest, SendsTypedTransportAndEchoesLargeInjectedAtom) {
    guitarrackcraft::LV2Plugin instance(plugin_, generation_, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);

    constexpr std::size_t kBodySize = 8193;
    std::vector<uint8_t> injected(sizeof(LV2_Atom) + kBodySize);
    const LV2_Atom header = {static_cast<uint32_t>(kBodySize), 0x12345678u};
    std::memcpy(injected.data(), &header, sizeof(header));
    for (std::size_t i = 0; i < kBodySize; ++i)
        injected[sizeof(LV2_Atom) + i] =
            static_cast<uint8_t>(11u + static_cast<uint8_t>(i * 29u));
    instance.injectAtom(injected.data(), static_cast<uint32_t>(injected.size()));

    constexpr std::size_t kBurstCount = 42;
    std::vector<std::vector<uint8_t>> burst;
    burst.reserve(kBurstCount);
    for (std::size_t index = 0; index < kBurstCount; ++index) {
        const uint32_t bodySize = static_cast<uint32_t>(4u + index % 13u);
        std::vector<uint8_t> atom(sizeof(LV2_Atom) + bodySize);
        const LV2_Atom atomHeader = {
            bodySize, 0x70000000u + static_cast<uint32_t>(index)};
        std::memcpy(atom.data(), &atomHeader, sizeof(atomHeader));
        for (std::size_t byte = 0; byte < bodySize; ++byte) {
            atom[sizeof(LV2_Atom) + byte] = static_cast<uint8_t>(
                0x31u + index * 17u + byte * 23u);
        }
        instance.injectAtom(atom.data(), static_cast<uint32_t>(atom.size()));
        burst.push_back(atom);
    }

    guitarrackcraft::AudioProcessContext context{};
    context.transportFrame = 123456789;
    context.playing = true;
    context.beatsPerMinute = 137.5;
    context.beatsPerBar = 7.0f;
    context.beatUnit = 8;
    context.bar = 42;
    context.barBeat = 2.5;
    std::vector<OutputAtomEvent> output;
    output.reserve(kBurstCount + 2);
    for (uint32_t tick = 0; tick < 6; ++tick) {
        const auto& processContext = tick == 0
            ? context
            : guitarrackcraft::AudioProcessContext{};
        ASSERT_EQ(processNoMidi(instance, nullptr, nullptr, 64, processContext),
                  guitarrackcraft::MidiOutputDisposition::Replace);
        auto current = instance.drainOutputAtoms();
        output.insert(output.end(), current.begin(), current.end());
    }
    ASSERT_EQ(output.size(), kBurstCount + 2);
    bool foundEcho = false;
    bool foundTypedTransportMarker = false;
    for (const OutputAtomEvent& event : output) {
        EXPECT_EQ(event.portIndex, 1u);
        if (event.data == injected) {
            foundEcho = true;
        }
        if (event.data.size() == sizeof(LV2_Atom) + sizeof(int32_t)) {
            int32_t marker = 0;
            std::memcpy(&marker, event.data.data() + sizeof(LV2_Atom),
                        sizeof(marker));
            if (marker == 0x4c563254) foundTypedTransportMarker = true;
        }
    }
    EXPECT_TRUE(foundEcho);
    EXPECT_TRUE(foundTypedTransportMarker);

    for (const auto& expected : burst) {
        std::size_t matches = 0;
        for (const auto& event : output)
            if (event.data == expected) ++matches;
        EXPECT_EQ(matches, 1u);
    }
    instance.deactivate();
}

TEST_F(LV2HostContractTest, PreservesTwoByteChannelVoiceAndThreeByteNoteMidi) {
    guitarrackcraft::LV2Plugin instance(plugin_, generation_, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);

    const uint8_t program[] = {0xc0, 12};
    const uint8_t pressure[] = {0xd0, 64};
    const uint8_t note[] = {0x90, 60, 100};
    guitarrackcraft::MidiBuffer inputMidi;
    ASSERT_TRUE(inputMidi.append(3, program, sizeof(program)));
    ASSERT_TRUE(inputMidi.append(11, pressure, sizeof(pressure)));
    ASSERT_TRUE(inputMidi.append(17, note, sizeof(note)));
    guitarrackcraft::MidiBuffer outputMidi;
    const auto disposition =
        instance.process(nullptr, nullptr, 64,
                         guitarrackcraft::AudioProcessContext{}, inputMidi,
                         outputMidi);

    ASSERT_EQ(disposition, guitarrackcraft::MidiOutputDisposition::Replace);
    ASSERT_EQ(outputMidi.eventCount(), 3u);
    EXPECT_EQ(outputMidi.eventAt(0).frameOffset, 3u);
    EXPECT_EQ(outputMidi.eventAt(1).frameOffset, 11u);
    EXPECT_EQ(outputMidi.eventAt(2).frameOffset, 17u);
    EXPECT_EQ(outputMidi.eventAt(0).payloadSize, sizeof(program));
    EXPECT_EQ(outputMidi.eventAt(1).payloadSize, sizeof(pressure));
    EXPECT_EQ(outputMidi.eventAt(2).payloadSize, sizeof(note));
    EXPECT_TRUE(std::equal(program, program + sizeof(program),
                           outputMidi.payloadFor(outputMidi.eventAt(0))));
    EXPECT_TRUE(std::equal(pressure, pressure + sizeof(pressure),
                           outputMidi.payloadFor(outputMidi.eventAt(1))));
    EXPECT_TRUE(std::equal(note, note + sizeof(note),
                           outputMidi.payloadFor(outputMidi.eventAt(2))));
    instance.deactivate();
}

TEST_F(LV2HostContractTest, EchoesShortAndMaximumSysExPayloadsByteExactly) {
    guitarrackcraft::LV2Plugin instance(plugin_, generation_, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);

    const std::array<uint8_t, 2> program = {0xc0, 12};
    guitarrackcraft::MidiBuffer inputMidi;
    ASSERT_TRUE(inputMidi.append(3, program.data(), program.size()));
    guitarrackcraft::MidiBuffer outputMidi;
    ASSERT_EQ(instance.process(nullptr, nullptr, 64,
                               guitarrackcraft::AudioProcessContext{},
                               inputMidi, outputMidi),
              guitarrackcraft::MidiOutputDisposition::Replace);
    ASSERT_EQ(outputMidi.eventCount(), 1u);
    EXPECT_EQ(outputMidi.eventAt(0).frameOffset, 3u);
    ASSERT_EQ(outputMidi.eventAt(0).payloadSize, program.size());
    EXPECT_TRUE(std::equal(program.begin(), program.end(),
                           outputMidi.payloadFor(outputMidi.eventAt(0))));

    std::vector<uint8_t> sysex(guitarrackcraft::kMaxMidiPayloadBytes);
    sysex.front() = 0xf0;
    for (uint32_t i = 1; i + 1 < sysex.size(); ++i)
        sysex[i] = static_cast<uint8_t>((i * 19u) % 127u);
    sysex.back() = 0xf7;
    inputMidi.clear();
    outputMidi.clear();
    ASSERT_TRUE(inputMidi.append(17, sysex.data(), sysex.size()));
    ASSERT_EQ(instance.process(nullptr, nullptr, 64,
                               guitarrackcraft::AudioProcessContext{},
                               inputMidi, outputMidi),
              guitarrackcraft::MidiOutputDisposition::Replace);
    ASSERT_EQ(outputMidi.eventCount(), 1u);
    const auto& actual = outputMidi.eventAt(0);
    EXPECT_EQ(actual.frameOffset, 17u);
    ASSERT_EQ(actual.payloadSize, sysex.size());
    EXPECT_TRUE(std::equal(sysex.begin(), sysex.end(),
                           outputMidi.payloadFor(actual)));
    instance.deactivate();
}


TEST_F(LV2HostContractTest, MIDIOutputPortIsAuthoritativeWhenItEmitsNoEvents) {
    guitarrackcraft::LV2Plugin instance(plugin_, generation_, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);
    guitarrackcraft::MidiBuffer inputMidi;
    guitarrackcraft::MidiBuffer outputMidi;
    EXPECT_EQ(instance.process(nullptr, nullptr, 64,
                               guitarrackcraft::AudioProcessContext{},
                               inputMidi, outputMidi),
              guitarrackcraft::MidiOutputDisposition::Replace);
    EXPECT_EQ(outputMidi.eventCount(), 0u);
    instance.deactivate();
}

TEST(LV2PluginProcessTest, RunsDspAndCopiesProcessedAudio) {
    ScopedEnvironment lv2Path("LV2_PATH", LV2_FIXTURE_DIR);

    LilvWorld* world = lilv_world_new();
    ASSERT_NE(world, nullptr);
    lilv_world_load_all(world);

    LilvNode* uri = lilv_new_uri(world, "https://guitarrackcraft.test/lv2/tiny-gain");
    auto generation = std::make_shared<const guitarrackcraft::LV2PluginGeneration>(world);
    ASSERT_NE(uri, nullptr);
    const LilvPlugin* plugin = lilv_world_get_all_plugins(world)
        ? lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri)
        : nullptr;
    ASSERT_NE(plugin, nullptr);

    {
        guitarrackcraft::LV2Plugin instance(plugin, generation, 48000.0f);
        ASSERT_TRUE(instance.hasInstance());
        ASSERT_EQ(instance.getNumInputPorts(), 1u);
        ASSERT_EQ(instance.getNumOutputPorts(), 1u);

        instance.activate(48000.0f, 64);

        constexpr uint32_t kFrames = 64;
        float input[kFrames];
        float outputLeft[kFrames];
        float outputRight[kFrames];
        for (uint32_t i = 0; i < kFrames; ++i) {
            input[i] = 0.125f + static_cast<float>(i) * 0.03125f;
            outputLeft[i] = -7.0f;
            outputRight[i] = -7.0f;
        }

        const float* inputs[2] = {input, nullptr};
        float* outputs[2] = {outputLeft, outputRight};
        const guitarrackcraft::AudioProcessContext context{};
        (void)processNoMidi(instance, inputs, outputs, kFrames, context);

        for (uint32_t i = 0; i < kFrames; ++i) {
            EXPECT_FLOAT_EQ(outputLeft[i], input[i] * 2.0f);
            EXPECT_FLOAT_EQ(outputRight[i], input[i] * 2.0f);
        }

        instance.deactivate();
    }

    lilv_node_free(uri);
}

namespace {

struct LilvFixture final {
    explicit LilvFixture(const char* pluginUri)
        : environment(std::make_unique<ScopedEnvironment>("LV2_PATH", LV2_FIXTURE_DIR)),
          world(lilv_world_new()),
          uri(world ? lilv_new_uri(world, pluginUri) : nullptr) {
        if (world) lilv_world_load_all(world);
        if (world) generation =
            std::make_shared<const guitarrackcraft::LV2PluginGeneration>(world);
        const LilvPlugins* plugins = world ? lilv_world_get_all_plugins(world) : nullptr;
        plugin = plugins && uri ? lilv_plugins_get_by_uri(plugins, uri) : nullptr;
    }

    ~LilvFixture() {
        if (uri) lilv_node_free(uri);
        generation.reset();
        world = nullptr;
    }

    std::unique_ptr<ScopedEnvironment> environment;
    LilvWorld* world = nullptr;
    LilvNode* uri = nullptr;
    const LilvPlugin* plugin = nullptr;
    std::shared_ptr<const guitarrackcraft::LV2PluginGeneration> generation;
};

std::vector<int32_t> drainIntEvents(guitarrackcraft::LV2Plugin& instance) {
    std::vector<int32_t> values;
    for (const OutputAtomEvent& event : instance.drainOutputAtoms()) {
        if (event.data.size() != sizeof(LV2_Atom) + sizeof(int32_t)) continue;
        int32_t value = 0;
        std::memcpy(&value, event.data.data() + sizeof(LV2_Atom), sizeof(value));
        values.push_back(value);
    }
    return values;
}

bool contains(const std::vector<int32_t>& values, int32_t expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

}  // namespace

TEST(LV2PluginProcessTest, ValidFixtureAdmissionAndActivationReadiness) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/tiny-gain");
    ASSERT_NE(fixture.plugin, nullptr);

    guitarrackcraft::LV2Plugin instance(
        fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    EXPECT_FALSE(instance.isReadyForRealtime());
    EXPECT_EQ(instance.getInfo().realtimeClass,
              guitarrackcraft::RealtimeClass::CertifiedInProcess);

    instance.activate(48000.0f, 64);
    EXPECT_TRUE(instance.isReadyForRealtime());
    EXPECT_EQ(instance.getInfo().realtimeClass,
              guitarrackcraft::RealtimeClass::CertifiedInProcess);

    instance.deactivate();
    EXPECT_FALSE(instance.isReadyForRealtime());
}

TEST(LV2PluginProcessTest, FactoryEnumerationAndCreationPreserveAdmissionMetadata) {
    guitarrackcraft::LV2PluginFactory factory(LV2_FIXTURE_DIR);
    ASSERT_TRUE(factory.initialize());

    const auto enumerated = factory.enumeratePlugins();
    const auto found = std::find_if(
        enumerated.begin(), enumerated.end(), [](const guitarrackcraft::PluginInfo& info) {
            return info.id == "https://guitarrackcraft.test/lv2/tiny-gain";
        });
    ASSERT_NE(found, enumerated.end());
    EXPECT_EQ(found->format, "LV2");
    EXPECT_EQ(found->realtimeClass,
              guitarrackcraft::RealtimeClass::CertifiedInProcess);

    auto created = factory.createPlugin(found->id);
    ASSERT_NE(created, nullptr);
    const auto createdInfo = created->getInfo();
    EXPECT_EQ(createdInfo.id, found->id);
    EXPECT_EQ(createdInfo.format, found->format);
    EXPECT_EQ(createdInfo.realtimeClass, found->realtimeClass);
    EXPECT_FALSE(created->isReadyForRealtime());

    created->activate(48000.0f, 64);
    EXPECT_TRUE(created->isReadyForRealtime());
    EXPECT_EQ(created->getInfo().realtimeClass,
              guitarrackcraft::RealtimeClass::CertifiedInProcess);
    created->deactivate();
}

TEST(LV2PluginProcessTest, InvalidActivationStaysUnreadyAndBypassesAudio) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/tiny-gain");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(
        fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());

    constexpr std::array<std::pair<float, uint32_t>, 2> invalidActivations{{
        {0.0f, 64u},
        {48000.0f, 8193u},
    }};
    constexpr uint32_t kFrames = 4;
    const float inputLeft[kFrames] = {0.25f, 0.5f, 0.75f, 1.0f};
    const float inputRight[kFrames] = {1.0f, 0.75f, 0.5f, 0.25f};
    const float* inputs[2] = {inputLeft, inputRight};
    float outputLeft[kFrames];
    float outputRight[kFrames];
    float* outputs[2] = {outputLeft, outputRight};

    for (const auto [sampleRate, bufferSize] : invalidActivations) {
        instance.activate(sampleRate, bufferSize);
        EXPECT_FALSE(instance.isReadyForRealtime());
        EXPECT_EQ(instance.getInfo().realtimeClass,
                  guitarrackcraft::RealtimeClass::Unsupported);
        std::fill(outputLeft, outputLeft + kFrames, -9.0f);
        std::fill(outputRight, outputRight + kFrames, -9.0f);
        (void)processNoMidi(instance, inputs, outputs, kFrames,
                            guitarrackcraft::AudioProcessContext{});
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            EXPECT_FLOAT_EQ(outputLeft[frame], inputLeft[frame]);
            EXPECT_FLOAT_EQ(outputRight[frame], inputRight[frame]);
        }
    }
}

TEST(LV2PluginProcessTest, UnsupportedRequiredFeatureFailsCleanly) {
    LilvFixture fixture(
        "https://guitarrackcraft.test/lv2/unsupported-required");
    ASSERT_NE(fixture.plugin, nullptr);

    guitarrackcraft::LV2Plugin instance(
        fixture.plugin, fixture.generation, 48000.0f);
    EXPECT_FALSE(instance.hasInstance());
    EXPECT_FALSE(instance.isReadyForRealtime());
    EXPECT_EQ(instance.getInfo().realtimeClass,
              guitarrackcraft::RealtimeClass::Unsupported);

    instance.activate(48000.0f, 64);
    EXPECT_FALSE(instance.isReadyForRealtime());
    EXPECT_FALSE(instance.hasInstance());

    constexpr uint32_t kFrames = 4;
    const float inputLeft[kFrames] = {0.1f, 0.2f, 0.3f, 0.4f};
    const float inputRight[kFrames] = {0.4f, 0.3f, 0.2f, 0.1f};
    const float* inputs[2] = {inputLeft, inputRight};
    float outputLeft[kFrames];
    float outputRight[kFrames];
    float* outputs[2] = {outputLeft, outputRight};
    (void)processNoMidi(instance, inputs, outputs, kFrames,
                        guitarrackcraft::AudioProcessContext{});
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft[frame], inputLeft[frame]);
        EXPECT_FLOAT_EQ(outputRight[frame], inputRight[frame]);
    }
}

TEST(LV2PluginProcessTest, PluginChainPublishesActivatedFixture) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/tiny-gain");
    ASSERT_NE(fixture.plugin, nullptr);
    auto plugin = std::make_unique<guitarrackcraft::LV2Plugin>(
        fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(plugin->hasInstance());

    guitarrackcraft::PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::move(plugin)), 0);
    ASSERT_EQ(chain.getSize(), 1u);
    ASSERT_TRUE(chain.visitPlugin(0, [](guitarrackcraft::IPlugin& admitted) {
        const auto info = admitted.getInfo();
        return info.id == "https://guitarrackcraft.test/lv2/tiny-gain" &&
               info.format == "LV2" &&
               info.realtimeClass ==
                   guitarrackcraft::RealtimeClass::CertifiedInProcess;
    }));

    chain.setSampleRate(48000.0f, 64);
    EXPECT_TRUE(chain.getRealtimeDiagnostic().empty());
    chain.activate();
    EXPECT_TRUE(chain.getRealtimeDiagnostic().empty());

    constexpr uint32_t kFrames = 4;
    const float inputLeft[kFrames] = {0.125f, 0.25f, 0.5f, 1.0f};
    const float inputRight[kFrames] = {1.0f, 0.5f, 0.25f, 0.125f};
    const float* inputs[2] = {inputLeft, inputRight};
    float outputLeft[kFrames] = {};
    float outputRight[kFrames] = {};
    float* outputs[2] = {outputLeft, outputRight};
    processChainNoMidi(chain, inputs, outputs, kFrames,
                       guitarrackcraft::AudioProcessContext{});
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft[frame], inputLeft[frame] * 2.0f);
        EXPECT_FLOAT_EQ(outputRight[frame], inputLeft[frame] * 2.0f);
    }
    chain.deactivate();
}

TEST(LV2PluginProcessTest, RuntimeUridFaultEnablesPersistentPassthrough) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/urid-fault");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(
        fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);
    ASSERT_TRUE(instance.isReadyForRealtime());

    constexpr uint32_t kFrames = 4;
    const float inputLeft[kFrames] = {0.2f, 0.4f, 0.6f, 0.8f};
    const float inputRight[kFrames] = {0.8f, 0.6f, 0.4f, 0.2f};
    const float* inputs[2] = {inputLeft, inputRight};
    float outputLeft[kFrames];
    float outputRight[kFrames];
    float* outputs[2] = {outputLeft, outputRight};

    for (int block = 0; block < 2; ++block) {
        std::fill(outputLeft, outputLeft + kFrames, -9.0f);
        std::fill(outputRight, outputRight + kFrames, -9.0f);
        (void)processNoMidi(instance, inputs, outputs, kFrames,
                            guitarrackcraft::AudioProcessContext{});
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            EXPECT_FLOAT_EQ(outputLeft[frame], inputLeft[frame]);
            EXPECT_FLOAT_EQ(outputRight[frame], inputRight[frame]);
        }
    }
    instance.deactivate();
}

TEST(LV2PluginProcessTest, ActivatedQuantumIsExactAndOversizedCallbacksDoNotProcessPartialTail) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/tiny-gain");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);

    std::array<float, 65> input{};
    std::array<float, 65> output{};
    for (uint32_t i = 0; i < input.size(); ++i) {
        input[i] = 0.25f + static_cast<float>(i) * 0.01f;
        output[i] = -99.0f;
    }
    const float* inputs[] = {input.data(), nullptr};
    float* outputs[] = {output.data(), nullptr};

    (void)processNoMidi(instance, inputs, outputs, 64,
                        guitarrackcraft::AudioProcessContext{});
    for (uint32_t i = 0; i < 64; ++i)
        EXPECT_FLOAT_EQ(output[i], input[i] * 2.0f);
    EXPECT_FLOAT_EQ(output[64], -99.0f);

    output.fill(-99.0f);
    (void)processNoMidi(instance, inputs, outputs, 65,
                        guitarrackcraft::AudioProcessContext{});
    for (uint32_t i = 0; i < input.size(); ++i)
        EXPECT_FLOAT_EQ(output[i], input[i]);
    instance.deactivate();
}

TEST(LV2PluginProcessTest, ActivationAndDeactivationAreAcknowledgedBeforeNextProcess) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/tiny-gain");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(
        fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    const float* inputs[2];
    float* outputs[2];
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    for (uint32_t i = 0; i < input.size(); ++i) input[i] = 0.5f + i;
    inputs[0] = input.data();
    inputs[1] = nullptr;
    outputs[0] = output.data();
    outputs[1] = nullptr;

    instance.activate(48000.0f, 32);
    (void)processNoMidi(instance, inputs, outputs, 32,
                        guitarrackcraft::AudioProcessContext{});
    EXPECT_FLOAT_EQ(output[31], input[31] * 2.0f);

    instance.activate(48000.0f, 64);
    (void)processNoMidi(instance, inputs, outputs, 64,
                        guitarrackcraft::AudioProcessContext{});
    EXPECT_FLOAT_EQ(output[63], input[63] * 2.0f);

    instance.deactivate();
    output.fill(-7.0f);
    (void)processNoMidi(instance, inputs, outputs, 64,
                        guitarrackcraft::AudioProcessContext{});
    for (uint32_t i = 0; i < input.size(); ++i)
        EXPECT_FLOAT_EQ(output[i], input[i]);
}

TEST_F(LV2HostContractTest, BoundedAtomQueueDropsAfterCapacityWithoutReorderingEarlierEvents) {
    guitarrackcraft::LV2Plugin instance(plugin_, generation_, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);

    constexpr uint32_t kQueuedAtoms = 300;
    constexpr uint32_t kExpectedAtoms = 256;
    constexpr uint32_t kBodySize = 4;
    for (uint32_t index = 0; index < kQueuedAtoms; ++index) {
        std::vector<uint8_t> atom(sizeof(LV2_Atom) + kBodySize);
        const LV2_Atom header = {kBodySize, 0x55000000u + index};
        std::memcpy(atom.data(), &header, sizeof(header));
        std::memcpy(atom.data() + sizeof(LV2_Atom), &index, sizeof(index));
        instance.injectAtom(atom.data(), static_cast<uint32_t>(atom.size()));
    }
    // The payload is deliberately over the fixture's declared 524288-byte
    // atom capacity.  It must be rejected without corrupting queued records.
    std::vector<uint8_t> oversized(524289, 0xa5);
    instance.injectAtom(oversized.data(), static_cast<uint32_t>(oversized.size()));

    std::vector<uint32_t> seen;
    for (uint32_t tick = 0; tick < 40; ++tick) {
        (void)processNoMidi(instance, nullptr, nullptr, 64,
                            guitarrackcraft::AudioProcessContext{});
        for (const OutputAtomEvent& event : instance.drainOutputAtoms()) {
            if (event.data.size() != sizeof(LV2_Atom) + kBodySize) continue;
            uint32_t value = 0;
            std::memcpy(&value, event.data.data() + sizeof(LV2_Atom), sizeof(value));
            seen.push_back(value);
        }
    }
    ASSERT_EQ(seen.size(), kExpectedAtoms);
    for (uint32_t index = 0; index < kExpectedAtoms; ++index)
        EXPECT_EQ(seen[index], index);
    instance.deactivate();
}

TEST_F(LV2HostContractTest, ConcurrentInstancesShareCollisionSafeURIDs) {
    constexpr uint32_t kInstances = 12;
    struct PerThreadWorld final {
        ~PerThreadWorld() {
            if (uri) lilv_node_free(uri);
            generation.reset();
            world = nullptr;
        }
        LilvWorld* world = nullptr;
        LilvNode* uri = nullptr;
        const LilvPlugin* plugin = nullptr;
        std::shared_ptr<const guitarrackcraft::LV2PluginGeneration> generation;
    };

    std::vector<PerThreadWorld> fixtures(kInstances);
    for (auto& fixture : fixtures) {
        fixture.world = lilv_world_new();
        ASSERT_NE(fixture.world, nullptr);
        lilv_world_load_all(fixture.world);
        fixture.uri = lilv_new_uri(
            fixture.world, "https://guitarrackcraft.test/lv2/host-contract");
        ASSERT_NE(fixture.uri, nullptr);
        fixture.generation =
            std::make_shared<const guitarrackcraft::LV2PluginGeneration>(fixture.world);
        const LilvPlugins* plugins = lilv_world_get_all_plugins(fixture.world);
        fixture.plugin = plugins
            ? lilv_plugins_get_by_uri(plugins, fixture.uri)
            : nullptr;
        ASSERT_NE(fixture.plugin, nullptr);
    }

    std::vector<std::thread> threads;
    std::vector<uint8_t> succeeded(kInstances, 0);
    std::atomic<uint32_t> ready{0};
    std::atomic<bool> go{false};
    threads.reserve(kInstances);
    for (uint32_t index = 0; index < kInstances; ++index) {
        threads.emplace_back([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto& fixture = fixtures[index];
            guitarrackcraft::LV2Plugin instance(
                fixture.plugin, fixture.generation, 48000.0f);
            if (!instance.hasInstance()) return;
            instance.activate(48000.0f, 64);
            guitarrackcraft::AudioProcessContext context{};
            context.transportFrame = 123456789;
            context.playing = true;
            context.beatsPerMinute = 137.5;
            context.beatsPerBar = 7.0f;
            context.beatUnit = 8;
            context.bar = 42;
            context.barBeat = 2.5;
            (void)processNoMidi(instance, nullptr, nullptr, 64, context);
            const auto values = drainIntEvents(instance);
            succeeded[index] = contains(values, 0x4c563254) ? 1 : 0;
            instance.deactivate();
        });
    }
    while (ready.load(std::memory_order_acquire) != kInstances) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    for (bool success : succeeded) EXPECT_TRUE(success);
}

TEST(LV2WorkerContractTest, OptionalEndRunIsAdmittedAndProcessesWorkerResponseAudio) {
    LilvFixture fixture(
        "https://guitarrackcraft.test/lv2/worker-contract-without-end-run");
    ASSERT_NE(fixture.plugin, nullptr);
    auto plugin = std::make_unique<guitarrackcraft::LV2Plugin>(
        fixture.plugin, fixture.generation, 48000.0f);
    auto* instance = plugin.get();

    guitarrackcraft::PluginChain chain;
    chain.setSampleRate(48000.0f, 64);
    ASSERT_EQ(chain.addPlugin(std::move(plugin)), 0);
    ASSERT_EQ(chain.getSize(), 1u);
    ASSERT_TRUE(instance->isReadyForRealtime());
    EXPECT_EQ(instance->getInfo().realtimeClass,
              guitarrackcraft::RealtimeClass::CertifiedInProcess);
    EXPECT_TRUE(chain.getRealtimeDiagnostic().empty());
    chain.activate();
    ASSERT_TRUE(chain.getRealtimeDiagnostic().empty());

    ASSERT_TRUE(chain.visitPlugin(0, [](guitarrackcraft::IPlugin& admitted) {
        admitted.setParameter(0, 1.0f);
        return true;
    }));

    constexpr uint32_t kFrames = 4;
    const float inputLeft[kFrames] = {0.125f, 0.25f, 0.5f, 1.0f};
    const float inputRight[kFrames] = {1.0f, 0.5f, 0.25f, 0.125f};
    const float* inputs[2] = {inputLeft, inputRight};
    float outputLeft[kFrames] = {};
    float outputRight[kFrames] = {};
    float* outputs[2] = {outputLeft, outputRight};

    processChainNoMidi(chain, inputs, outputs, kFrames,
                       guitarrackcraft::AudioProcessContext{});
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft[frame], inputLeft[frame]);
        EXPECT_FLOAT_EQ(outputRight[frame], inputLeft[frame]);
    }

    bool sawResponse = false;
    bool sawProcessedAudio = false;
    for (uint32_t tick = 0; tick < 128 && !sawProcessedAudio; ++tick) {
        std::fill(outputLeft, outputLeft + kFrames, -7.0f);
        std::fill(outputRight, outputRight + kFrames, -7.0f);
        processChainNoMidi(chain, inputs, outputs, kFrames,
                           guitarrackcraft::AudioProcessContext{});
        const auto values = drainIntEvents(*instance);
        if (contains(values, 10201)) sawResponse = true;
        bool scaled = true;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            scaled = scaled &&
                     outputLeft[frame] == inputLeft[frame] * 2.0f &&
                     outputRight[frame] == inputLeft[frame] * 2.0f;
        }
        if (sawResponse && scaled) sawProcessedAudio = true;
        if (!sawProcessedAudio) std::this_thread::yield();
    }
    EXPECT_TRUE(sawResponse);
    EXPECT_TRUE(sawProcessedAudio);
    chain.deactivate();
}

TEST(LV2WorkerContractTest, OptionalWorkResponseIsAdmittedAndKeepsProcessing) {
    LilvFixture fixture(
        "https://guitarrackcraft.test/lv2/worker-contract-work-only");
    ASSERT_NE(fixture.plugin, nullptr);
    auto plugin = std::make_unique<guitarrackcraft::LV2Plugin>(
        fixture.plugin, fixture.generation, 48000.0f);
    auto* instance = plugin.get();

    guitarrackcraft::PluginChain chain;
    chain.setSampleRate(48000.0f, 64);
    ASSERT_EQ(chain.addPlugin(std::move(plugin)), 0);
    ASSERT_TRUE(instance->isReadyForRealtime());
    chain.activate();
    ASSERT_TRUE(chain.getRealtimeDiagnostic().empty());

    // Schedule work the plugin will never answer: with work_response absent
    // the response is dropped, and the run cycle must carry on regardless.
    ASSERT_TRUE(chain.visitPlugin(0, [](guitarrackcraft::IPlugin& admitted) {
        admitted.setParameter(0, 1.0f);
        return true;
    }));

    constexpr uint32_t kFrames = 4;
    const float inputLeft[kFrames] = {0.125f, 0.25f, 0.5f, 1.0f};
    const float inputRight[kFrames] = {1.0f, 0.5f, 0.25f, 0.125f};
    const float* inputs[2] = {inputLeft, inputRight};
    float outputLeft[kFrames] = {};
    float outputRight[kFrames] = {};
    float* outputs[2] = {outputLeft, outputRight};

    for (uint32_t tick = 0; tick < 16; ++tick) {
        processChainNoMidi(chain, inputs, outputs, kFrames,
                           guitarrackcraft::AudioProcessContext{});
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            EXPECT_FLOAT_EQ(outputLeft[frame], inputLeft[frame]);
            EXPECT_FLOAT_EQ(outputRight[frame], inputLeft[frame]);
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(chain.getRealtimeDiagnostic().empty());
    chain.deactivate();
}

TEST(LV2WorkerContractTest, WorkerResponseIsDeliveredBeforeEndRunWhenAvailable) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/worker-contract");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);
    instance.setParameter(0, 1.0f);

    bool sawResponse = false;
    for (uint32_t tick = 0; tick < 128 && !sawResponse; ++tick) {
        (void)processNoMidi(instance, nullptr, nullptr, 64,
                            guitarrackcraft::AudioProcessContext{});
        const auto values = drainIntEvents(instance);
        const auto response = std::find(values.begin(), values.end(), 10201);
        if (response != values.end()) {
            sawResponse = true;
            const auto end = std::find(values.begin(), values.end(), 30000);
            ASSERT_NE(end, values.end());
            EXPECT_LT(response, end);
        }
        if (!sawResponse) std::this_thread::yield();
    }
    EXPECT_TRUE(sawResponse);
    instance.deactivate();
}

TEST(LV2WorkerContractTest, FullRequestQueueReturnsNoSpaceInsteadOfBlockingOrReordering) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/worker-contract");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);
    instance.setParameter(0, 3.0f);

    std::vector<int32_t> values;
    for (uint32_t tick = 0; tick < 8; ++tick) {
        (void)processNoMidi(instance, nullptr, nullptr, 64,
                            guitarrackcraft::AudioProcessContext{});
        const auto current = drainIntEvents(instance);
        values.insert(values.end(), current.begin(), current.end());
        std::this_thread::yield();
    }
    auto accepted = std::find_if(values.begin(), values.end(),
                                 [](int32_t value) { return value >= 3000 && value < 3100; });
    auto rejected = std::find_if(values.begin(), values.end(),
                                 [](int32_t value) { return value >= 3100 && value < 3200; });
    ASSERT_NE(accepted, values.end());
    ASSERT_NE(rejected, values.end());
    EXPECT_EQ((*accepted - 3000) + (*rejected - 3100), 128);
    EXPECT_GT(*rejected, 3100);
    instance.deactivate();
}

TEST(LV2WorkerContractTest, ResponseQueueAndPayloadBoundariesReturnNoSpaceWithDropBudget) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/worker-contract");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);

    instance.setParameter(0, 2.0f);
    std::vector<int32_t> responseValues;
    bool sawResponseDrop = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(250);
    uint32_t tick = 0;
    while (std::chrono::steady_clock::now() < deadline &&
           (responseValues.empty() || !sawResponseDrop)) {
        (void)processNoMidi(instance, nullptr, nullptr, 64,
                            guitarrackcraft::AudioProcessContext{});
        for (int32_t value : drainIntEvents(instance)) {
            if (value >= 12000 && value < 12200) responseValues.push_back(value);
            if (value > 3200) sawResponseDrop = true;
        }
        if (tick++ == 0) instance.setParameter(0, 0.0f);
        if (responseValues.empty() || !sawResponseDrop)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_FALSE(responseValues.empty());
    for (int32_t value : responseValues) {
        EXPECT_GE(value, 12000);
        EXPECT_LT(value, 12200);
    }
    EXPECT_TRUE(sawResponseDrop);

    instance.setParameter(0, 4.0f);
    bool sawOversizedDrop = false;
    for (uint32_t tick = 0; tick < 128 && !sawOversizedDrop; ++tick) {
        (void)processNoMidi(instance, nullptr, nullptr, 64,
                            guitarrackcraft::AudioProcessContext{});
        for (int32_t value : drainIntEvents(instance))
            if (value >= 3201) sawOversizedDrop = true;
        if (!sawOversizedDrop) std::this_thread::yield();
    }
    EXPECT_TRUE(sawOversizedDrop);
    instance.deactivate();
}
