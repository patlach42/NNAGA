#include <gtest/gtest.h>

#include "plugin/lv2/LV2Plugin.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>


#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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
        ASSERT_EQ(instance.process(nullptr, nullptr, 64, processContext,
                                   nullptr, 0, nullptr, 0), 0u);
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

    ASSERT_EQ(instance.process(inputs, outputs, 64,
                               guitarrackcraft::AudioProcessContext{},
                               nullptr, 0, nullptr, 0), 0u);
    for (uint32_t i = 0; i < 64; ++i)
        EXPECT_FLOAT_EQ(output[i], input[i] * 2.0f);
    EXPECT_FLOAT_EQ(output[64], -99.0f);

    output.fill(-99.0f);
    ASSERT_EQ(instance.process(inputs, outputs, 65,
                               guitarrackcraft::AudioProcessContext{},
                               nullptr, 0, nullptr, 0), 0u);
    for (uint32_t i = 0; i < input.size(); ++i)
        EXPECT_FLOAT_EQ(output[i], input[i]);
    instance.deactivate();
}

TEST(LV2PluginProcessTest, ActivationAndDeactivationAreAcknowledgedBeforeNextProcess) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/tiny-gain");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(fixture.plugin, fixture.generation, 48000.0f);
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
    ASSERT_EQ(instance.process(inputs, outputs, 32,
                               guitarrackcraft::AudioProcessContext{},
                               nullptr, 0, nullptr, 0), 0u);
    EXPECT_FLOAT_EQ(output[31], input[31] * 2.0f);

    instance.activate(48000.0f, 64);
    ASSERT_EQ(instance.process(inputs, outputs, 64,
                               guitarrackcraft::AudioProcessContext{},
                               nullptr, 0, nullptr, 0), 0u);
    EXPECT_FLOAT_EQ(output[63], input[63] * 2.0f);

    instance.deactivate();
    output.fill(-7.0f);
    ASSERT_EQ(instance.process(inputs, outputs, 64,
                               guitarrackcraft::AudioProcessContext{},
                               nullptr, 0, nullptr, 0), 0u);
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
        ASSERT_EQ(instance.process(nullptr, nullptr, 64,
                                   guitarrackcraft::AudioProcessContext{},
                                   nullptr, 0, nullptr, 0), 0u);
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
            instance.process(nullptr, nullptr, 64, context, nullptr, 0, nullptr, 0);
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

TEST(LV2WorkerContractTest, WorkerResponseIsDeliveredBeforeEndRunWhenAvailable) {
    LilvFixture fixture("https://guitarrackcraft.test/lv2/worker-contract");
    ASSERT_NE(fixture.plugin, nullptr);
    guitarrackcraft::LV2Plugin instance(fixture.plugin, fixture.generation, 48000.0f);
    ASSERT_TRUE(instance.hasInstance());
    instance.activate(48000.0f, 64);
    instance.setParameter(0, 1.0f);

    bool sawResponse = false;
    for (uint32_t tick = 0; tick < 128 && !sawResponse; ++tick) {
        ASSERT_EQ(instance.process(nullptr, nullptr, 64,
                                   guitarrackcraft::AudioProcessContext{},
                                   nullptr, 0, nullptr, 0), 0u);
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
        ASSERT_EQ(instance.process(nullptr, nullptr, 64,
                                   guitarrackcraft::AudioProcessContext{},
                                   nullptr, 0, nullptr, 0), 0u);
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
    for (uint32_t tick = 0; tick < 128; ++tick) {
        ASSERT_EQ(instance.process(nullptr, nullptr, 64,
                                   guitarrackcraft::AudioProcessContext{},
                                   nullptr, 0, nullptr, 0), 0u);
        for (int32_t value : drainIntEvents(instance)) {
            if (value >= 12000 && value < 12200) responseValues.push_back(value);
            if (value > 3200) sawResponseDrop = true;
        }
        if (tick == 0) instance.setParameter(0, 0.0f);
        std::this_thread::yield();
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
        ASSERT_EQ(instance.process(nullptr, nullptr, 64,
                                   guitarrackcraft::AudioProcessContext{},
                                   nullptr, 0, nullptr, 0), 0u);
        for (int32_t value : drainIntEvents(instance))
            if (value >= 3201) sawOversizedDrop = true;
        if (!sawOversizedDrop) std::this_thread::yield();
    }
    EXPECT_TRUE(sawOversizedDrop);
    instance.deactivate();
}
