#include "plugin/PluginChain.h"
#include "plugin/native/NativePlugin.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {
using guitarrackcraft::AudioProcessContext;
using guitarrackcraft::NativePlugin;
using guitarrackcraft::NativePluginLibrary;
using guitarrackcraft::PluginChain;
using guitarrackcraft::RealtimeClass;

bool activationSucceeds = true;
uint32_t activationCalls = 0;
double lastActivationSampleRate = 0.0;
uint32_t lastActivationQuantum = 0;
uint32_t processCalls = 0;
float publishedParameter = 0.0f;

void* createPlugin() noexcept { return reinterpret_cast<void*>(static_cast<uintptr_t>(1)); }
void destroyPlugin(NnagaPluginHandle) noexcept {}
int32_t activatePlugin(NnagaPluginHandle, double sampleRate, uint32_t quantum) noexcept {
    ++activationCalls;
    lastActivationSampleRate = sampleRate;
    lastActivationQuantum = quantum;
    return activationSucceeds ? 1 : 0;
}
void deactivatePlugin(NnagaPluginHandle) noexcept {}
void resetPlugin(NnagaPluginHandle) noexcept {}
void setParameter(NnagaPluginHandle, uint32_t, float value) noexcept { publishedParameter = value; }
void processPlugin(NnagaPluginHandle, const float* inputLeft, const float* inputRight, float* outputLeft,
                   float* outputRight, uint32_t frames, const NnagaProcessContextV2*) noexcept {
    ++processCalls;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        outputLeft[frame] = inputLeft[frame] * publishedParameter;
        outputRight[frame] = inputRight[frame] * publishedParameter;
    }
}

NnagaParameterV2 parameterStorage{
    sizeof(NnagaParameterV2), 4, "Gain", "gain", "", 0, 0.5f, 0, nullptr, 0,
};
NnagaPluginDescriptorV2 descriptor{
    sizeof(NnagaPluginDescriptorV2), "com.example.thirdparty", "third-party", "Third-party", "Test vendor", "1.0",
    2, 2, 1, 512, NNAGA_REALTIME_CERTIFIED_IN_PROCESS, &parameterStorage, createPlugin, destroyPlugin, activatePlugin,
    deactivatePlugin, resetPlugin, setParameter, nullptr, nullptr, processPlugin,
};

const NnagaPluginDescriptorV2* getPlugin(uint32_t index) noexcept {
    return index == 0 ? &descriptor : nullptr;
}

NnagaPluginLibraryV2 libraryAbi{
    sizeof(NnagaPluginLibraryV2), NNAGA_NATIVE_ABI_VERSION, 1, getPlugin,
};

std::shared_ptr<NativePluginLibrary> library() {
    auto result = std::make_shared<NativePluginLibrary>();
    result->abi = &libraryAbi;
    result->path = "test-native-plugin";
    return result;
}

void restoreDescriptor() {
    descriptor = {
        sizeof(NnagaPluginDescriptorV2), "com.example.thirdparty", "third-party", "Third-party", "Test vendor", "1.0",
        2, 2, 1, 512, NNAGA_REALTIME_CERTIFIED_IN_PROCESS, &parameterStorage, createPlugin, destroyPlugin, activatePlugin,
        deactivatePlugin, resetPlugin, setParameter, nullptr, nullptr, processPlugin,
    };
    libraryAbi = {sizeof(NnagaPluginLibraryV2), NNAGA_NATIVE_ABI_VERSION, 1, getPlugin};
    activationSucceeds = true;
    activationCalls = 0;
    lastActivationSampleRate = 0.0;
    lastActivationQuantum = 0;
    processCalls = 0;
    publishedParameter = 0.0f;
}

class NativePluginHostTest : public ::testing::Test {
protected:
    void SetUp() override { restoreDescriptor(); }
};

TEST_F(NativePluginHostTest, AcceptsWellFormedAbiV2Descriptor) {
    std::vector<const NnagaPluginDescriptorV2*> descriptors;
    std::string error;
    ASSERT_TRUE(guitarrackcraft::validateNativePluginLibrary(library(), &descriptors, &error));
    ASSERT_EQ(descriptors.size(), 1u);
    EXPECT_EQ(descriptors[0], &descriptor);
    EXPECT_TRUE(error.empty());
}

TEST_F(NativePluginHostTest, RejectsMalformedAbiV2DescriptorFields) {
    struct Case {
        const char* name;
        void (*mutate)();
    };
    const Case cases[] = {
        {"missing alias", [] { descriptor.alias = ""; }},
        {"invalid alias characters", [] { descriptor.alias = "not/portable"; }},
        {"negative realtime class", [] {
             descriptor.realtime_class = static_cast<NnagaRealtimeClassV2>(-1);
         }},
        {"unknown realtime class", [] {
             descriptor.realtime_class = static_cast<NnagaRealtimeClassV2>(99);
         }},
        {"zero max frames", [] { descriptor.max_frames = 0; }},
        {"oversize max frames", [] { descriptor.max_frames = NNAGA_NATIVE_MAX_FRAMES + 1u; }},
        {"short descriptor struct", [] { descriptor.struct_size = sizeof(NnagaPluginDescriptorV2) - 1u; }},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.name);
        restoreDescriptor();
        test.mutate();
        std::string error;
        EXPECT_FALSE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, &error));
        EXPECT_EQ(error, "invalid plugin descriptor");
    }
}

TEST_F(NativePluginHostTest, RejectsMalformedLibraryAbiV2) {
    libraryAbi.abi_version = NNAGA_NATIVE_ABI_VERSION - 1u;
    std::string error;
    EXPECT_FALSE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, &error));
    EXPECT_EQ(error, "invalid library ABI");

    restoreDescriptor();
    libraryAbi.plugin_count = 0;
    error.clear();
    EXPECT_FALSE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, &error));
    EXPECT_EQ(error, "invalid library ABI");
}

TEST_F(NativePluginHostTest, ArbitraryCertifiedIdExposesCertifiedClass) {
    descriptor.id = "com.vendor.arbitrary-certified";
    ASSERT_TRUE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, nullptr));
    NativePlugin plugin(library(), &descriptor);
    EXPECT_EQ(plugin.getInfo().realtimeClass, RealtimeClass::CertifiedInProcess);
}

TEST_F(NativePluginHostTest, UnsupportedDescriptorStaysUnsupported) {
    descriptor.realtime_class = NNAGA_REALTIME_UNSUPPORTED;
    ASSERT_TRUE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, nullptr));
    NativePlugin plugin(library(), &descriptor);
    EXPECT_EQ(plugin.getInfo().realtimeClass, RealtimeClass::Unsupported);
}

TEST_F(NativePluginHostTest, IsolatedNativeDescriptorIsNotAdmittedInProcess) {
    descriptor.realtime_class = NNAGA_REALTIME_ISOLATED;
    ASSERT_TRUE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, nullptr));
    NativePlugin plugin(library(), &descriptor);
    EXPECT_EQ(plugin.getInfo().realtimeClass, RealtimeClass::Unsupported);
}

TEST_F(NativePluginHostTest, ActivationReadinessRequiresSuccessWithinDeclaredQuantum) {
    NativePlugin plugin(library(), &descriptor);
    EXPECT_FALSE(plugin.isReadyForRealtime());

    activationSucceeds = false;
    plugin.activate(48000.0f, 256);
    EXPECT_FALSE(plugin.isReadyForRealtime());

    activationSucceeds = true;
    plugin.activate(48000.0f, descriptor.max_frames + 1);
    EXPECT_FALSE(plugin.isReadyForRealtime());

    plugin.activate(48000.0f, 0);
    EXPECT_FALSE(plugin.isReadyForRealtime());

    plugin.activate(48000.0f, descriptor.max_frames);
    EXPECT_TRUE(plugin.isReadyForRealtime());
    EXPECT_DOUBLE_EQ(lastActivationSampleRate, 48000.0);
    EXPECT_EQ(lastActivationQuantum, descriptor.max_frames);
}

TEST_F(NativePluginHostTest, ChainPublishesAndProcessesArbitraryCertifiedNativePlugin) {
    descriptor.id = "com.vendor.arbitrary-certified";
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<NativePlugin>(library(), &descriptor)), 0);

    chain.setSampleRate(48000.0f, 4);
    ASSERT_EQ(activationCalls, 1u);
    EXPECT_DOUBLE_EQ(lastActivationSampleRate, 48000.0);
    EXPECT_EQ(lastActivationQuantum, 4u);
    chain.activate();

    const uint64_t instanceId = chain.getPluginInstanceId(0);
    ASSERT_NE(instanceId, 0u);
    ASSERT_TRUE(chain.submitParameter(instanceId, 4, 0.75f));

    constexpr uint32_t frames = 4;
    const float inputLeft[frames] = {1.0f, 0.5f, -1.0f, 0.25f};
    const float inputRight[frames] = {-0.5f, 1.0f, 0.25f, -1.0f};
    float outputLeft[frames] = {};
    float outputRight[frames] = {};
    const float* inputs[] = {inputLeft, inputRight};
    float* outputs[] = {outputLeft, outputRight};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    ASSERT_EQ(chain.process(inputs, outputs, frames, context, nullptr, 0, nullptr, 0), 0u);
    EXPECT_EQ(processCalls, 1u);
    EXPECT_FLOAT_EQ(outputLeft[0], 0.75f);
    EXPECT_FLOAT_EQ(outputLeft[1], 0.375f);
    EXPECT_FLOAT_EQ(outputRight[0], -0.375f);
    EXPECT_FLOAT_EQ(outputRight[3], -0.75f);
}

TEST_F(NativePluginHostTest, UnreadyPluginReportsDiagnosticWithoutRackMutation) {
    PluginChain chain;
    chain.setSampleRate(48000.0f, 256);
    activationSucceeds = false;

    EXPECT_EQ(chain.addPlugin(std::make_unique<NativePlugin>(library(), &descriptor)), -1);
    EXPECT_EQ(chain.getSize(), 0u);
    EXPECT_EQ(chain.getRealtimeDiagnostic(), "plugin-activate-failed");
}
} // namespace
