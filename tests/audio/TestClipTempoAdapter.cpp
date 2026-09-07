/*
 * Copyright (C) 2026 patlach42
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>

#include "plugin/ClipTempoAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace {

using guitarrackcraft::ClipTempoAdapter;
using guitarrackcraft::ClipTempoMode;

std::vector<float> ramp(size_t frames) {
    std::vector<float> data(frames);
    for (size_t i = 0; i < frames; ++i) data[i] = 1.0f - static_cast<float>(i) / frames;
    return data;
}

std::vector<float> renderFrames(const ClipTempoAdapter& adapter,
                                const std::vector<float>& source,
                                uint64_t firstFrame, uint32_t frames) {
    std::vector<float> rendered(frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        float right = 0.0f;
        adapter.renderStereo(source.data(), nullptr, source.size(),
                             firstFrame + frame, rendered[frame], right);
    }
    return rendered;
}

double sourceIndexForFrame(uint64_t outputFrame, uint32_t sourceRate,
                           uint32_t outputRate, ClipTempoMode mode,
                           double ratio) {
    const double rateScale = static_cast<double>(sourceRate) / outputRate;
    return static_cast<double>(outputFrame) * rateScale *
           (mode == ClipTempoMode::Original ? 1.0 : ratio);
}

// Silence past the end is the contract, not a bug: a loop region longer than
// the audio leaves silence until the next boundary. Softening this edge would
// hide a seam whose real cause is a loop length that disagrees with the sample
// count, which is chosen upstream.
TEST(ClipTempoAdapter, LeavesSilencePastTheEndOfTheSource) {
    const auto data = ramp(64);
    ClipTempoAdapter adapter;
    adapter.configure(ClipTempoMode::Original, 120.0, 120.0, 48000, 48000);

    float last = 0.0f, unused = 0.0f;
    adapter.renderStereo(data.data(), nullptr, data.size(), 63, last, unused);
    EXPECT_NEAR(last, data[63], 1e-6f);

    for (uint64_t position : {uint64_t{64}, uint64_t{65}, uint64_t{4096}}) {
        float value = 1.0f;
        adapter.renderStereo(data.data(), nullptr, data.size(), position, value, unused);
        EXPECT_FLOAT_EQ(value, 0.0f) << "position " << position;
    }
}

TEST(ClipTempoAdapter, RejectsEmptyAndNullSources) {
    ClipTempoAdapter adapter;
    adapter.configure(ClipTempoMode::Original, 120.0, 120.0, 48000, 48000);
    float l = 1.0f, r = 1.0f;
    adapter.renderStereo(nullptr, nullptr, 0, 0, l, r);
    EXPECT_FLOAT_EQ(l, 0.0f);
    EXPECT_FLOAT_EQ(r, 0.0f);

    const auto data = ramp(4);
    l = 1.0f;
    adapter.renderStereo(data.data(), nullptr, 0, 0, l, r);
    EXPECT_FLOAT_EQ(l, 0.0f);
}

// A phase-continuous tone must survive a wrap unchanged: this is the property
// the listening probe relied on, so a regression here invalidates that test.
TEST(ClipTempoAdapter, PhaseContinuousToneIsUnchangedAtTheWrap) {
    constexpr size_t kFrames = 4800;  // 44 whole cycles of 440 Hz at 48 kHz
    std::vector<float> tone(kFrames);
    for (size_t i = 0; i < kFrames; ++i)
        tone[i] = static_cast<float>(std::sin(2.0 * M_PI * 440.0 * i / 48000.0));

    ClipTempoAdapter adapter;
    adapter.configure(ClipTempoMode::Original, 120.0, 120.0, 48000, 48000);

    float lastOfLoop = 0.0f, firstOfNext = 0.0f, unused = 0.0f;
    adapter.renderStereo(tone.data(), nullptr, kFrames, kFrames - 1, lastOfLoop, unused);
    adapter.renderStereo(tone.data(), nullptr, kFrames, 0, firstOfNext, unused);

    // One sample step of a 440 Hz sine at 48 kHz spans well under 0.06.
    EXPECT_LT(std::fabs(firstOfNext - lastOfLoop), 0.06f)
        << "the wrap introduced a step of " << (firstOfNext - lastOfLoop);
}

TEST(ClipTempoAdapter, RatiosPreserveModeLengthAndSourceIndexSlope) {
    constexpr uint32_t sourceRate = 44'100;
    constexpr uint32_t outputRate = 48'000;
    constexpr size_t sourceFrames = 8192;
    std::vector<float> source(sourceFrames);
    for (size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = static_cast<float>(frame);

    struct Case {
        ClipTempoMode mode;
        double ratio;
    };
    const std::array<Case, 5> cases = {{
        {ClipTempoMode::Original, 1.0},
        {ClipTempoMode::Stretch, 0.75},
        {ClipTempoMode::Stretch, 1.25},
        {ClipTempoMode::Repitch, 0.75},
        {ClipTempoMode::Repitch, 1.25},
    }};
    for (const auto& testCase : cases) {
        SCOPED_TRACE(static_cast<int>(testCase.mode));
        SCOPED_TRACE(testCase.ratio);
        ClipTempoAdapter adapter;
        adapter.configure(testCase.mode, 120.0, 120.0 * testCase.ratio,
                          sourceRate, outputRate);

        const double expectedLength =
            sourceFrames * static_cast<double>(outputRate) / sourceRate /
            (testCase.mode == ClipTempoMode::Original ? 1.0 : testCase.ratio);
        EXPECT_NEAR(adapter.adaptedLengthFrames(sourceFrames), expectedLength, 1e-9);
        for (const uint64_t outputFrame : {uint64_t{0}, uint64_t{511},
                                           uint64_t{512}, uint64_t{513},
                                           uint64_t{1023}, uint64_t{1024},
                                           uint64_t{1025}}) {
            float rendered = 0.0f;
            float unused = 0.0f;
            adapter.renderStereo(source.data(), nullptr, source.size(),
                                 outputFrame, rendered, unused);
            const double expected = sourceIndexForFrame(
                outputFrame, sourceRate, outputRate, testCase.mode,
                testCase.ratio);
            // Stretch uses a 512-frame grain: its source-index markers are
            // contractual at grain boundaries, while the interior is a
            // crossfade used to preserve pitch.
            const bool markerBoundary =
                testCase.mode != ClipTempoMode::Stretch || outputFrame % 512 == 0;
            if (markerBoundary) {
                EXPECT_NEAR(rendered, expected, 1.0)
                    << "output frame " << outputFrame;
            }
        }
    }
}

TEST(ClipTempoAdapter, HopBoundaryAndCallbackPartitionDoNotChangeRenderedFrames) {
    constexpr uint32_t sourceRate = 44'100;
    constexpr uint32_t outputRate = 48'000;
    constexpr uint32_t totalFrames = 1536;
    std::vector<float> source(8192);
    for (size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = static_cast<float>(frame);

    ClipTempoAdapter adapter;
    adapter.configure(ClipTempoMode::Stretch, 120.0, 90.0,
                      sourceRate, outputRate);
    const auto contiguous = renderFrames(adapter, source, 0, totalFrames);

    std::vector<float> partitioned;
    partitioned.reserve(totalFrames);
    const std::array<uint32_t, 4> blocks = {{127u, 128u, 511u, 513u}};
    uint32_t rendered = 0;
    for (size_t blockIndex = 0; rendered < totalFrames; ++blockIndex) {
        const uint32_t block = blocks[blockIndex % blocks.size()];
        const uint32_t count = std::min(block, totalFrames - rendered);
        const auto chunk = renderFrames(adapter, source, rendered, count);
        partitioned.insert(partitioned.end(), chunk.begin(), chunk.end());
        rendered += count;
    }
    ASSERT_EQ(partitioned.size(), contiguous.size());
    for (uint32_t frame = 0; frame < totalFrames; ++frame) {
        EXPECT_NEAR(partitioned[frame], contiguous[frame], 1e-5f)
            << "callback partition changed frame " << frame;
    }

    // The hop seam is an observable marker boundary, not a permission to drop
    // or duplicate a frame. A ramp therefore remains monotonic through 511/512/513.
    for (const uint32_t frame : {511u, 512u, 513u}) {
        EXPECT_LE(contiguous[frame - 1], contiguous[frame]);
        EXPECT_LE(contiguous[frame], contiguous[frame + 1]);
    }
}

} // namespace
