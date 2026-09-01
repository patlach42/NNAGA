#include "plugin/native/NativePlugin.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

namespace {
using guitarrackcraft::NativePlugin;
using guitarrackcraft::NativePluginLibrary;
using guitarrackcraft::RealtimeClass;

void* createPlugin() noexcept { return reinterpret_cast<void*>(static_cast<uintptr_t>(1)); }
void destroyPlugin(NnagaPluginHandle) noexcept {}
int32_t activatePlugin(NnagaPluginHandle, double, uint32_t) noexcept { return 1; }
void deactivatePlugin(NnagaPluginHandle) noexcept {}
void resetPlugin(NnagaPluginHandle) noexcept {}
void setParameter(NnagaPluginHandle, uint32_t, float) noexcept {}
void processPlugin(NnagaPluginHandle, const float*, const float*, float*, float*, uint32_t,
                  const NnagaProcessContextV2*) noexcept {}

NnagaParameterV2 parameterStorage{
    sizeof(NnagaParameterV2), 4, "Gain", "gain", "", 0, 0.5f, 0, nullptr, 0,
};
NnagaPluginDescriptorV2 descriptor{
    sizeof(NnagaPluginDescriptorV2), "com.example.thirdparty", "third-party", "Third-party", "Test vendor", "1.0",
    2, 2, 0, 512, NNAGA_REALTIME_CERTIFIED_IN_PROCESS, &parameterStorage, createPlugin, destroyPlugin, activatePlugin,
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
        2, 2, 0, 512, NNAGA_REALTIME_CERTIFIED_IN_PROCESS, &parameterStorage, createPlugin, destroyPlugin, activatePlugin,
        deactivatePlugin, resetPlugin, setParameter, nullptr, nullptr, processPlugin,
    };
    libraryAbi = {sizeof(NnagaPluginLibraryV2), NNAGA_NATIVE_ABI_VERSION, 1, getPlugin};
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
        {"invalid realtime class", [] {
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

TEST_F(NativePluginHostTest, ThirdPartyCertifiedClaimIsNotExposedAsInProcess) {
    ASSERT_TRUE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, nullptr));
    NativePlugin plugin(library(), &descriptor);
    EXPECT_EQ(plugin.getInfo().realtimeClass, RealtimeClass::Unsupported);
}

TEST_F(NativePluginHostTest, AuditedNativeIdMayExposeCertifiedClass) {
    descriptor.id = "com.vibes.dsp.filter";
    ASSERT_TRUE(guitarrackcraft::validateNativePluginLibrary(library(), nullptr, nullptr));
    NativePlugin plugin(library(), &descriptor);
    EXPECT_EQ(plugin.getInfo().realtimeClass, RealtimeClass::CertifiedInProcess);
}
}
