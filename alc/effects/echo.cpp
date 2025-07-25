/**
 * OpenAL cross platform audio library
 * Copyright (C) 2009 by Chris Robinson.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <span>
#include <variant>
#include <vector>

#include "alc/effects/base.h"
#include "alnumeric.h"
#include "core/ambidefs.h"
#include "core/bufferline.h"
#include "core/context.h"
#include "core/device.h"
#include "core/effects/base.h"
#include "core/effectslot.h"
#include "core/filters/biquad.h"
#include "core/mixer.h"
#include "intrusive_ptr.h"
#include "opthelpers.h"

struct BufferStorage;

namespace {

using uint = unsigned int;

constexpr float LowpassFreqRef{5000.0f};

struct EchoState final : public EffectState {
    std::vector<float> mSampleBuffer;

    // The echo is two tap. The delay is the number of samples from before the
    // current offset
    std::array<size_t,2> mDelayTap{};
    size_t mOffset{0u};

    /* The panning gains for the two taps */
    struct OutGains {
        std::array<float,MaxAmbiChannels> Current{};
        std::array<float,MaxAmbiChannels> Target{};
    };
    std::array<OutGains,2> mGains;

    BiquadFilter mFilter;
    float mFeedGain{0.0f};

    alignas(16) std::array<FloatBufferLine,2> mTempBuffer{};

    void deviceUpdate(const DeviceBase *device, const BufferStorage *buffer) override;
    void update(const ContextBase *context, const EffectSlot *slot, const EffectProps *props,
        const EffectTarget target) override;
    void process(const size_t samplesToDo, const std::span<const FloatBufferLine> samplesIn,
        const std::span<FloatBufferLine> samplesOut) override;
};

void EchoState::deviceUpdate(const DeviceBase *Device, const BufferStorage*)
{
    const auto frequency = static_cast<float>(Device->mSampleRate);

    // Use the next power of 2 for the buffer length, so the tap offsets can be
    // wrapped using a mask instead of a modulo
    const uint maxlen{NextPowerOf2(float2uint(EchoMaxDelay*frequency + 0.5f) +
        float2uint(EchoMaxLRDelay*frequency + 0.5f))};
    if(maxlen != mSampleBuffer.size())
        decltype(mSampleBuffer)(maxlen).swap(mSampleBuffer);

    std::ranges::fill(mSampleBuffer, 0.0f);
    mGains.fill(OutGains{});
}

void EchoState::update(const ContextBase *context, const EffectSlot *slot,
    const EffectProps *props_, const EffectTarget target)
{
    auto &props = std::get<EchoProps>(*props_);
    const auto *device = context->mDevice;
    const auto frequency = static_cast<float>(device->mSampleRate);

    mDelayTap[0] = std::max(float2uint(std::round(props.Delay*frequency)), 1u);
    mDelayTap[1] = float2uint(std::round(props.LRDelay*frequency)) + mDelayTap[0];

    const auto gainhf = std::max(1.0f - props.Damping, 0.0625f); /* Limit -24dB */
    mFilter.setParamsFromSlope(BiquadType::HighShelf, LowpassFreqRef/frequency, gainhf, 1.0f);

    mFeedGain = props.Feedback;

    /* Convert echo spread (where 0 = center, +/-1 = sides) to a 2D vector. */
    const auto x = props.Spread; /* +x = left */
    const auto z = std::sqrt(1.0f - x*x);

    const auto coeffs0 = CalcAmbiCoeffs( x, 0.0f, z, 0.0f);
    const auto coeffs1 = CalcAmbiCoeffs(-x, 0.0f, z, 0.0f);

    mOutTarget = target.Main->Buffer;
    ComputePanGains(target.Main, coeffs0, slot->Gain, mGains[0].Target);
    ComputePanGains(target.Main, coeffs1, slot->Gain, mGains[1].Target);
}

void EchoState::process(const size_t samplesToDo, const std::span<const FloatBufferLine> samplesIn,
    const std::span<FloatBufferLine> samplesOut)
{
    const auto delaybuf = std::span{mSampleBuffer};
    const auto mask = delaybuf.size()-1;
    auto offset = mOffset;
    auto tap1 = offset - mDelayTap[0];
    auto tap2 = offset - mDelayTap[1];

    ASSUME(samplesToDo > 0);

    const auto filter = mFilter;
    auto [z1, z2] = mFilter.getComponents();
    for(auto i=0_uz;i < samplesToDo;)
    {
        offset &= mask;
        tap1 &= mask;
        tap2 &= mask;

        const auto max_offset = std::max(offset, std::max(tap1, tap2));
        auto td = std::min(mask+1 - max_offset, samplesToDo-i);
        do {
            /* Feed the delay buffer's input first. */
            delaybuf[offset] = samplesIn[0][i];

            /* Get delayed output from the first and second taps. Use the
             * second tap for feedback.
             */
            mTempBuffer[0][i] = delaybuf[tap1++];
            mTempBuffer[1][i] = delaybuf[tap2++];
            const auto feedb = mTempBuffer[1][i++];

            /* Add feedback to the delay buffer with damping and attenuation. */
            delaybuf[offset++] += filter.processOne(feedb, z1, z2) * mFeedGain;
        } while(--td);
    }
    mFilter.setComponents(z1, z2);
    mOffset = offset;

    for(size_t c{0};c < 2;c++)
        MixSamples(std::span{mTempBuffer[c]}.first(samplesToDo), samplesOut, mGains[c].Current,
            mGains[c].Target, samplesToDo, 0);
}


struct EchoStateFactory final : public EffectStateFactory {
    al::intrusive_ptr<EffectState> create() override
    { return al::intrusive_ptr<EffectState>{new EchoState{}}; }
};

} // namespace

auto EchoStateFactory_getFactory() -> gsl::not_null<EffectStateFactory*>
{
    static EchoStateFactory EchoFactory{};
    return &EchoFactory;
}
