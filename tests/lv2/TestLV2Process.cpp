#include <gtest/gtest.h>

#include "plugin/lv2/LV2Plugin.h"

#include <lilv/lilv.h>

#include <cmath>
#include <cstdlib>
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

}  // namespace

TEST(LV2PluginProcessTest, RunsDspAndCopiesProcessedAudio) {
    ScopedEnvironment lv2Path("LV2_PATH", LV2_FIXTURE_DIR);

    LilvWorld* world = lilv_world_new();
    ASSERT_NE(world, nullptr);
    lilv_world_load_all(world);

    LilvNode* uri = lilv_new_uri(world, "https://guitarrackcraft.test/lv2/tiny-gain");
    ASSERT_NE(uri, nullptr);
    const LilvPlugin* plugin = lilv_world_get_all_plugins(world)
        ? lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri)
        : nullptr;
    ASSERT_NE(plugin, nullptr);

    {
        guitarrackcraft::LV2Plugin instance(plugin, world, 48000.0f);
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
    lilv_world_free(world);
}
