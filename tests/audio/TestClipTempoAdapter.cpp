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

} // namespace
