#include <gtest/gtest.h>

#include "plugin/lv2/LV2Plugin.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>


#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <string>

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
    ASSERT_EQ(instance.process(nullptr, nullptr, 64, context,
                               nullptr, 0, nullptr, 0), 0u);

    const std::vector<OutputAtomEvent> output = instance.drainOutputAtoms();
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

    const guitarrackcraft::MidiEvent input[] = {
        {3, 0xc0, 12, 0x7e},
        {11, 0xd0, 64, 0x6a},
        {17, 0x90, 60, 100},
    };
    guitarrackcraft::MidiEvent output[3]{};
    const uint32_t outputCount =
        instance.process(nullptr, nullptr, 64, guitarrackcraft::AudioProcessContext{},
                         input, 3, output, 3);

    ASSERT_EQ(outputCount, 3u);
    EXPECT_EQ(output[0].frameOffset, 3u);
    EXPECT_EQ(output[0].status, 0xc0u);
    EXPECT_EQ(output[0].data1, 12u);
    EXPECT_EQ(output[0].data2, 0u);
    EXPECT_EQ(output[1].frameOffset, 11u);
    EXPECT_EQ(output[1].status, 0xd0u);
    EXPECT_EQ(output[1].data1, 64u);
    EXPECT_EQ(output[1].data2, 0u);
    EXPECT_EQ(output[2].frameOffset, 17u);
    EXPECT_EQ(output[2].status, 0x90u);
    EXPECT_EQ(output[2].data1, 60u);
    EXPECT_EQ(output[2].data2, 100u);
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
        ASSERT_EQ(instance.process(inputs, outputs, kFrames, context,
                                   nullptr, 0, nullptr, 0), 0u);

        for (uint32_t i = 0; i < kFrames; ++i) {
            EXPECT_FLOAT_EQ(outputLeft[i], input[i] * 2.0f);
            EXPECT_FLOAT_EQ(outputRight[i], input[i] * 2.0f);
        }

        instance.deactivate();
    }

    lilv_node_free(uri);
}
