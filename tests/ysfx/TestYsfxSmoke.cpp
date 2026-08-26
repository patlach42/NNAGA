#include <array>
#include <vector>
#include <gtest/gtest.h>
#include <ysfx.h>

#ifndef NNAGA_SMOKE_PATH
#error "NNAGA_SMOKE_PATH must point at the bundled smoke effect"
#endif

namespace {

class YsfxScope {
public:
    YsfxScope() : config_(ysfx_config_new()), effect_(ysfx_new(config_)) {}
    ~YsfxScope() {
        if (effect_)
            ysfx_free(effect_);
        if (config_)
            ysfx_config_free(config_);
    }

    ysfx_config_t* config() const { return config_; }
    ysfx_t* effect() const { return effect_; }

private:
    ysfx_config_t* config_;
    ysfx_t* effect_;
};

TEST(YsfxSmokeContractTest, LoadsCompilesAndExposesNnagaPortsSlidersAndGfx) {
    YsfxScope scope;
    ASSERT_NE(scope.config(), nullptr);
    ASSERT_NE(scope.effect(), nullptr);
    ysfx_register_builtin_audio_formats(scope.config());

    ASSERT_TRUE(ysfx_load_file(scope.effect(), NNAGA_SMOKE_PATH, 0));
    EXPECT_STREQ(ysfx_get_name(scope.effect()), "NNAGA Builtin Smoke");
    ASSERT_EQ(ysfx_get_num_inputs(scope.effect()), 2u);
    ASSERT_EQ(ysfx_get_num_outputs(scope.effect()), 2u);
    EXPECT_STREQ(ysfx_get_input_name(scope.effect(), 0), "L");
    EXPECT_STREQ(ysfx_get_input_name(scope.effect(), 1), "R");
    EXPECT_STREQ(ysfx_get_output_name(scope.effect(), 0), "L");
    EXPECT_STREQ(ysfx_get_output_name(scope.effect(), 1), "R");

    ASSERT_TRUE(ysfx_has_section(scope.effect(), ysfx_section_gfx));
    uint32_t gfx_dim[2] = {};
    ASSERT_TRUE(ysfx_get_gfx_dim(scope.effect(), gfx_dim));
    EXPECT_EQ(gfx_dim[0], 640u);
    EXPECT_EQ(gfx_dim[1], 360u);

    ASSERT_TRUE(ysfx_slider_exists(scope.effect(), 0));
    EXPECT_STREQ(ysfx_slider_get_name(scope.effect(), 0), "Gain (dB)");
    ysfx_slider_range_t gain_range{};
    ASSERT_TRUE(ysfx_slider_get_range(scope.effect(), 0, &gain_range));
    EXPECT_DOUBLE_EQ(gain_range.def, 0.0);
    EXPECT_DOUBLE_EQ(gain_range.min, -24.0);
    EXPECT_DOUBLE_EQ(gain_range.max, 24.0);
    EXPECT_DOUBLE_EQ(gain_range.inc, 0.1);
    EXPECT_FALSE(ysfx_slider_is_enum(scope.effect(), 0));

    ASSERT_TRUE(ysfx_compile(scope.effect(), 0));
    EXPECT_TRUE(ysfx_is_compiled(scope.effect()));
}

TEST(YsfxSmokeContractTest, ProcessesStereoAtZeroDbAndRendersInteractiveGfx) {
    YsfxScope scope;
    ASSERT_NE(scope.config(), nullptr);
    ASSERT_NE(scope.effect(), nullptr);
    ysfx_register_builtin_audio_formats(scope.config());
    ASSERT_TRUE(ysfx_load_file(scope.effect(), NNAGA_SMOKE_PATH, 0));
    ASSERT_TRUE(ysfx_compile(scope.effect(), 0));
    ysfx_init(scope.effect());

    ASSERT_TRUE(ysfx_slider_exists(scope.effect(), 0));
    EXPECT_DOUBLE_EQ(ysfx_slider_get_value(scope.effect(), 0), 0.0);

    constexpr uint32_t frames = 8;
    const std::array<float, frames> input_left = {
        -1.0f, -0.5f, -0.125f, 0.0f, 0.125f, 0.5f, 0.75f, 1.0f};
    const std::array<float, frames> input_right = {
        0.9f, 0.6f, 0.3f, 0.1f, -0.1f, -0.3f, -0.6f, -0.9f};
    std::array<float, frames> output_left{};
    std::array<float, frames> output_right{};
    const float* inputs[] = {input_left.data(), input_right.data()};
    float* outputs[] = {output_left.data(), output_right.data()};
    ysfx_process_float(scope.effect(), inputs, outputs, 2, 2, frames);
    for (uint32_t i = 0; i < frames; ++i) {
        EXPECT_NEAR(output_left[i], input_left[i], 1e-12);
        EXPECT_NEAR(output_right[i], input_right[i], 1e-12);
    }

    constexpr uint32_t width = 640;
    constexpr uint32_t height = 360;
    std::vector<uint8_t> pixels(width * height * 4, 0);
    ysfx_gfx_config_t gfx{};
    gfx.pixel_width = width;
    gfx.pixel_height = height;
    gfx.pixel_stride = width * 4;
    gfx.pixels = pixels.data();
    gfx.scale_factor = 1.0;
    ysfx_gfx_setup(scope.effect(), &gfx);
    ysfx_gfx_set_window_state(scope.effect(), true, true, true);
    ASSERT_TRUE(ysfx_gfx_run(scope.effect()));

    bool changed = false;
    for (uint8_t pixel : pixels)
        changed = changed || pixel != 0;
    EXPECT_TRUE(changed);

    const ysfx_real before_mouse = ysfx_slider_get_value(scope.effect(), 0);
    ysfx_gfx_update_mouse(scope.effect(), 0, 550, 55, 1, 0.0, 0.0);
    ASSERT_TRUE(ysfx_gfx_run(scope.effect()));
    EXPECT_GT(ysfx_slider_get_value(scope.effect(), 0), before_mouse);
}

} // namespace
