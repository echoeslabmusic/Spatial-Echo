// TapeDelayTestAccess: Test-only access to TapeDelayProcessor internals (see the header).

#include "TapeDelayTestAccess.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include <algorithm>
#include <cmath>

float TapeDelayTestAccess::feedbackCompensationGain(const TapeDelayProcessor& tape) noexcept
{
    if (!tape.feedbackCompensationEnabled)
        return 1.0f;
    return std::min(tape.getFeedbackGainCompensation(tape.feedbackAvgLevel[0]),
                    tape.getFeedbackGainCompensation(tape.feedbackAvgLevel[1]));
}

void TapeDelayTestAccess::poisonTapeBuffer(TapeDelayProcessor& tape, float value)
{
    // A short run of samples so every playback head crosses poisoned tape even
    // with interpolated reads. Timing shift from the extra writes is irrelevant
    // here - the point is to corrupt the loop, then watch the watchdog fix it.
    constexpr int kPoisonRunSamples = 64;
    const int poisonChannels = std::min(tape.NumChannels, static_cast<int>(tape.TapeBuffer.size()));
    for (size_t channel = 0; channel < static_cast<size_t>(poisonChannels); ++channel)
        if (tape.TapeBuffer[channel])
            for (int i = 0; i < kPoisonRunSamples; ++i)
                tape.TapeBuffer[channel]->BufferWrite(value);
}

void TapeDelayTestAccess::enableHeadTimingCapture(TapeDelayProcessor& tape, bool enabled)
{
    tape.captureHeadTiming = enabled;
    if (enabled)
    {
        tape.headTimingCaptured = false;
        tape.headTimingSnapshot = TapeDelayProcessor::HeadTimingSnapshot{};
        tape.headTimingSampleCounter = 0;
    }
}

bool TapeDelayTestAccess::getHeadTimingSnapshot(const TapeDelayProcessor& tape,
                                                TapeDelayProcessor::HeadTimingSnapshot& out)
{
    if (!tape.headTimingCaptured || !tape.headTimingSnapshot.valid)
        return false;
    out = tape.headTimingSnapshot;
    return true;
}

void TapeDelayTestAccess::processRecordChainOnly(TapeDelayProcessor& tape, std::vector<std::vector<float>>& buffer,
                                                 int blockSize)
{
    if (static_cast<int>(buffer.size()) != tape.NumChannels)
        tape.NumChannels = static_cast<int>(buffer.size());

    const int numCh = std::min(tape.NumChannels, static_cast<int>(buffer.size()));
    if (numCh <= 0)
        return;

    int availableSamples = blockSize;
    for (size_t ch = 0; ch < static_cast<size_t>(numCh); ++ch)
        availableSamples = std::min(availableSamples, static_cast<int>(buffer[ch].size()));
    if (availableSamples <= 0)
        return;

    if (numCh == 1)
    {
        for (size_t s = 0; s < static_cast<size_t>(availableSamples); ++s)
        {
            const float preSatWrite = buffer[0][s];
            tape.updateLoopNoiseGate(std::abs(preSatWrite));
            const float recordIn = tape.processRecordFilters(preSatWrite, 0);
            float writeSample = tape.recordPointSaturation(recordIn, 0);
            buffer[0][s] = tape.processDCBlock(writeSample, 0);
        }
        return;
    }

    for (size_t s = 0; s < static_cast<size_t>(availableSamples); ++s)
    {
        const float preSatWriteL = buffer[0][s];
        const float preSatWriteR = buffer[1][s];
        const float loopExcitation = std::max(std::abs(preSatWriteL), std::abs(preSatWriteR));
        tape.updateLoopNoiseGate(loopExcitation);

        const float recordInL = tape.processRecordFilters(preSatWriteL, 0);
        const float recordInR = tape.processRecordFilters(preSatWriteR, 1);
        float writeL = recordInL;
        float writeR = recordInR;
        tape.recordPointSaturationStereo(writeL, writeR);

        buffer[0][s] = tape.processDCBlock(writeL, 0);
        buffer[1][s] = tape.processDCBlock(writeR, 1);
    }
}

void TapeDelayTestAccess::processPlaybackOnly(TapeDelayProcessor& tape, std::vector<std::vector<float>>& buffer,
                                              int blockSize)
{
    if (static_cast<int>(buffer.size()) != tape.NumChannels)
        tape.NumChannels = static_cast<int>(buffer.size());

    if (tape.NumChannels <= 0)
        return;

    const int noiseWriteLimit = std::min(blockSize, static_cast<int>(tape.outputNoisePerSample.size()));
    if (noiseWriteLimit < blockSize)
        jassertfalse;
    if (noiseWriteLimit > 0)
        std::fill(tape.outputNoisePerSample.begin(), tape.outputNoisePerSample.begin() + noiseWriteLimit, 0.0f);

    if (tape.NumChannels >= 2)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            float inputL = buffer[0][static_cast<size_t>(sample)];
            float inputR = buffer[1][static_cast<size_t>(sample)];

            tape.DelayEnabledGain =
                tape.rampToward(tape.DelayEnabledGain, tape.DelayEnabledTarget, tape.delayEnableStep);
            tape.EchoSendGain = tape.rampToward(tape.EchoSendGain, tape.EchoSendTarget, tape.echoSendStep);

            tape.updateTempoSyncSmoothedDelays();
            float delayTimeSmoothedMotor = tape.processTransportDelayMs(0);

            float inputAvg = (inputL + inputR) * 0.5f;
            tape.NoiseProcessor.processSample(inputAvg);
            if (sample < noiseWriteLimit)
                tape.outputNoisePerSample[static_cast<size_t>(sample)] = tape.NoiseProcessor.getOutputNoise();
            float dropoutGain = tape.NoiseProcessor.getDropoutGain();

            float WF_L = tape.WFProcessors[0]->processSample();
            float WF_R = tape.WFProcessors[1]->processSample();

            float spliceWowBurst = tape.calculateSpliceWowBurst();
            WF_L += spliceWowBurst;
            WF_R += spliceWowBurst;
            WF_L = tape.shapeDelayModulation(WF_L, 0);
            WF_R = tape.shapeDelayModulation(WF_R, 1);

            float delayTimeSmoothedL = delayTimeSmoothedMotor;
            float delayTimeSmoothedR = tape.processTransportDelayMs(1);

            auto clampDelay = [](float ms)
            {
                if (std::isnan(ms) || std::isinf(ms))
                    return 70.0f;
                return std::max(1.0f, std::min(ms, 6000.0f));
            };

            float baseL[3] = {0.0f, 0.0f, 0.0f};
            float baseR[3] = {0.0f, 0.0f, 0.0f};
            if (tape.TempoSyncEnabled)
            {
                baseL[0] = tape.Head1SyncSmoothed;
                baseL[1] = tape.Head2SyncSmoothed;
                baseL[2] = tape.Head3SyncSmoothed;
                baseR[0] = baseL[0];
                baseR[1] = baseL[1];
                baseR[2] = baseL[2];
            }
            else
            {
                baseL[0] = delayTimeSmoothedL;
                baseL[1] = delayTimeSmoothedL * tape.HEAD2_RATIO;
                baseL[2] = delayTimeSmoothedL * tape.HEAD3_RATIO;
                baseR[0] = delayTimeSmoothedR;
                baseR[1] = delayTimeSmoothedR * tape.HEAD2_RATIO;
                baseR[2] = delayTimeSmoothedR * tape.HEAD3_RATIO;
            }

            float modL[3] = {0.0f, 0.0f, 0.0f};
            float modR[3] = {0.0f, 0.0f, 0.0f};
            for (int head = 0; head < 3; ++head)
            {
                modL[head] = tape.scaleDelayModulationForHead(WF_L, baseL[head], delayTimeSmoothedL);
                modR[head] = tape.scaleDelayModulationForHead(WF_R, baseR[head], delayTimeSmoothedR);
            }

            float ynL_PH[3] = {0.0f, 0.0f, 0.0f};
            float ynR_PH[3] = {0.0f, 0.0f, 0.0f};

            const float* targetBase = tape.syncCrossfadeTargetMs;
            const float prevBase[3] = {tape.syncCrossfadePrevMs[0], tape.syncCrossfadePrevMs[1],
                                       tape.syncCrossfadePrevMs[2]};

            for (int head = 0; head < 3; ++head)
            {
                if (tape.TempoSyncEnabled && tape.syncCrossfadeActive[head])
                {
                    const float fade = tape.lookupCosineFade(tape.syncCrossfadeProgress[head]);
                    float oldDelayL = clampDelay(
                        prevBase[head] + tape.scaleDelayModulationForHead(WF_L, prevBase[head], delayTimeSmoothedL));
                    float newDelayL = clampDelay(targetBase[head] + tape.scaleDelayModulationForHead(
                                                                        WF_L, targetBase[head], delayTimeSmoothedL));
                    float oldDelayR = clampDelay(
                        prevBase[head] + tape.scaleDelayModulationForHead(WF_R, prevBase[head], delayTimeSmoothedR));
                    float newDelayR = clampDelay(targetBase[head] + tape.scaleDelayModulationForHead(
                                                                        WF_R, targetBase[head], delayTimeSmoothedR));
                    const float oldL = tape.TapeBuffer[0]->BufferRead(oldDelayL, true);
                    const float newL = tape.TapeBuffer[0]->BufferRead(newDelayL, true);
                    const float oldR = tape.TapeBuffer[1]->BufferRead(oldDelayR, true);
                    const float newR = tape.TapeBuffer[1]->BufferRead(newDelayR, true);
                    ynL_PH[head] = oldL + (newL - oldL) * fade;
                    ynR_PH[head] = oldR + (newR - oldR) * fade;

                    tape.advanceSyncCrossfade(head);
                }
                else
                {
                    float delayL = clampDelay(baseL[head] + modL[head]);
                    float delayR = clampDelay(baseR[head] + modR[head]);
                    ynL_PH[head] = tape.TapeBuffer[0]->BufferRead(delayL, true);
                    ynR_PH[head] = tape.TapeBuffer[1]->BufferRead(delayR, true);
                }
            }

            float ynL_PH1 = ynL_PH[0];
            float ynL_PH2 = ynL_PH[1];
            float ynL_PH3 = ynL_PH[2];
            float ynR_PH1 = ynR_PH[0];
            float ynR_PH2 = ynR_PH[1];
            float ynR_PH3 = ynR_PH[2];

            float loopTimeSec = (tape.LOOP_TIME_RATIO * delayTimeSmoothedMotor) / 1000.0f;
            float phaseIncrement = (loopTimeSec > 0.0f) ? (1.0f / (loopTimeSec * tape.SampleRate)) : 0.0f;
            tape.tapePhase += phaseIncrement;
            if (tape.tapePhase >= 1.0f)
                tape.tapePhase -= 1.0f;

            const float spliceEnv = tape.calculateSpliceGain();

            float tapeGain = spliceEnv * dropoutGain;
            ynL_PH1 *= tapeGain;
            ynL_PH2 *= tapeGain;
            ynL_PH3 *= tapeGain;
            ynR_PH1 *= tapeGain;
            ynR_PH2 *= tapeGain;
            ynR_PH3 *= tapeGain;
            tape.applyOutputNoiseTapeGain(sample, tapeGain);

            if (!tape.headEnableInitialized)
            {
                tape.headEnableGain[0] = tape.headEnableTarget[0];
                tape.headEnableGain[1] = tape.headEnableTarget[1];
                tape.headEnableGain[2] = tape.headEnableTarget[2];
                tape.headEnableInitialized = true;
            }
            for (int h = 0; h < 3; ++h)
            {
                if (tape.headEnableGain[h] < tape.headEnableTarget[h])
                    tape.headEnableGain[h] =
                        std::min(tape.headEnableTarget[h], tape.headEnableGain[h] + tape.headEnableStep);
                else if (tape.headEnableGain[h] > tape.headEnableTarget[h])
                    tape.headEnableGain[h] =
                        std::max(tape.headEnableTarget[h], tape.headEnableGain[h] - tape.headEnableStep);
            }
            const float h1Gain = tape.headEnableGain[0];
            const float h2Gain = tape.headEnableGain[1];
            const float h3Gain = tape.headEnableGain[2];
            const std::array<float, 3> audibleHeadGains{h1Gain, h2Gain, h3Gain};
            const std::array<std::array<float, 2>, 3> audibleHeadSamples{
                {{{ynL_PH1, ynR_PH1}}, {{ynL_PH2, ynR_PH2}}, {{ynL_PH3, ynR_PH3}}}};

            if (tape.crossfadeActive)
            {
                tape.crossfadeProgress += tape.crossfadeStep;
                if (tape.crossfadeProgress >= 1.0f)
                {
                    tape.crossfadeProgress = 1.0f;
                    tape.crossfadeActive = false;
                    tape.PreviousStereoMode = tape.StereoMode;
                }
            }

            const float audibleKillGain = tape.killActive ? tape.killEnvelope : 1.0f;
            float outputL = 0.0f;
            float outputR = 0.0f;
            for (int head = 0; head < 3; ++head)
            {
                const float treatedL = tape.processAudibleHeadSample(audibleHeadSamples[static_cast<size_t>(head)][0] *
                                                                         audibleHeadGains[static_cast<size_t>(head)],
                                                                     head, 0, audibleKillGain, tape.DelayEnabledGain);
                const float treatedR = tape.processAudibleHeadSample(audibleHeadSamples[static_cast<size_t>(head)][1] *
                                                                         audibleHeadGains[static_cast<size_t>(head)],
                                                                     head, 1, audibleKillGain, tape.DelayEnabledGain);
                if (tape.perHeadCaptureEnabled)
                {
                    tape.storePerHeadCaptureSample(head, 0, sample, treatedL);
                    tape.storePerHeadCaptureSample(head, 1, sample, treatedR);
                }
                outputL += treatedL;
                outputR += treatedR;
            }

            buffer[0][static_cast<size_t>(sample)] = outputL;
            buffer[1][static_cast<size_t>(sample)] = outputR;

            if (tape.killActive)
            {
                tape.killEnvelope -= tape.killStep;
                if (tape.killEnvelope <= 0.0f)
                {
                    tape.killEnvelope = 0.0f;
                    tape.killActive = false;
                    tape.clearBuffers();
                }
            }

            float gatedInputL = inputL * tape.EchoSendGain;
            float gatedInputR = inputR * tape.EchoSendGain;
            if (tape.killActive)
            {
                gatedInputL *= tape.killEnvelope;
                gatedInputR *= tape.killEnvelope;
            }

            tape.TapeBuffer[0]->BufferWrite(gatedInputL);
            tape.TapeBuffer[1]->BufferWrite(gatedInputR);
        }
    }
    else
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            float xn = buffer[0][static_cast<size_t>(sample)];

            tape.DelayEnabledGain =
                tape.rampToward(tape.DelayEnabledGain, tape.DelayEnabledTarget, tape.delayEnableStep);
            tape.EchoSendGain = tape.rampToward(tape.EchoSendGain, tape.EchoSendTarget, tape.echoSendStep);

            tape.updateTempoSyncSmoothedDelays();
            float delayTimeSmoothed = tape.processTransportDelayMs(0);
            tape.NoiseProcessor.processSample(xn);
            if (sample < noiseWriteLimit)
                tape.outputNoisePerSample[static_cast<size_t>(sample)] = tape.NoiseProcessor.getOutputNoise();
            float dropoutGain = tape.NoiseProcessor.getDropoutGain();
            float WF = tape.WFProcessors[0]->processSample() + tape.calculateSpliceWowBurst();
            WF = tape.shapeDelayModulation(WF, 0);

            auto clampDelayMono = [](float ms)
            {
                if (std::isnan(ms) || std::isinf(ms))
                    return 70.0f;
                return std::max(1.0f, std::min(ms, 6000.0f));
            };

            float base[3] = {0.0f, 0.0f, 0.0f};
            if (tape.TempoSyncEnabled)
            {
                base[0] = tape.Head1SyncSmoothed;
                base[1] = tape.Head2SyncSmoothed;
                base[2] = tape.Head3SyncSmoothed;
            }
            else
            {
                base[0] = delayTimeSmoothed;
                base[1] = delayTimeSmoothed * tape.HEAD2_RATIO;
                base[2] = delayTimeSmoothed * tape.HEAD3_RATIO;
            }

            float mod[3] = {0.0f, 0.0f, 0.0f};
            for (int head = 0; head < 3; ++head)
                mod[head] = tape.scaleDelayModulationForHead(WF, base[head], delayTimeSmoothed);

            float ynPH[3] = {0.0f, 0.0f, 0.0f};
            const float* targetBase = tape.syncCrossfadeTargetMs;
            const float prevBase[3] = {tape.syncCrossfadePrevMs[0], tape.syncCrossfadePrevMs[1],
                                       tape.syncCrossfadePrevMs[2]};

            for (int head = 0; head < 3; ++head)
            {
                if (tape.TempoSyncEnabled && tape.syncCrossfadeActive[head])
                {
                    const float fade = tape.lookupCosineFade(tape.syncCrossfadeProgress[head]);
                    float oldDelay = clampDelayMono(
                        prevBase[head] + tape.scaleDelayModulationForHead(WF, prevBase[head], delayTimeSmoothed));
                    float newDelay = clampDelayMono(
                        targetBase[head] + tape.scaleDelayModulationForHead(WF, targetBase[head], delayTimeSmoothed));
                    const float oldSample = tape.TapeBuffer[0]->BufferRead(oldDelay, true);
                    const float newSample = tape.TapeBuffer[0]->BufferRead(newDelay, true);
                    ynPH[head] = oldSample + (newSample - oldSample) * fade;

                    tape.advanceSyncCrossfade(head);
                }
                else
                {
                    float delay = clampDelayMono(base[head] + mod[head]);
                    ynPH[head] = tape.TapeBuffer[0]->BufferRead(delay, true);
                }
            }

            float ynPH1 = ynPH[0];
            float ynPH2 = ynPH[1];
            float ynPH3 = ynPH[2];

            float loopTimeSec = (tape.LOOP_TIME_RATIO * delayTimeSmoothed) / 1000.0f;
            float phaseIncrement = (loopTimeSec > 0.0f) ? (1.0f / (loopTimeSec * tape.SampleRate)) : 0.0f;
            tape.tapePhase += phaseIncrement;
            if (tape.tapePhase >= 1.0f)
                tape.tapePhase -= 1.0f;

            const float spliceEnv = tape.calculateSpliceGain();

            float tapeGain = spliceEnv * dropoutGain;
            ynPH1 *= tapeGain;
            ynPH2 *= tapeGain;
            ynPH3 *= tapeGain;
            tape.applyOutputNoiseTapeGain(sample, tapeGain);

            if (!tape.headEnableInitialized)
            {
                tape.headEnableGain[0] = tape.headEnableTarget[0];
                tape.headEnableGain[1] = tape.headEnableTarget[1];
                tape.headEnableGain[2] = tape.headEnableTarget[2];
                tape.headEnableInitialized = true;
            }
            for (int h = 0; h < 3; ++h)
            {
                if (tape.headEnableGain[h] < tape.headEnableTarget[h])
                    tape.headEnableGain[h] =
                        std::min(tape.headEnableTarget[h], tape.headEnableGain[h] + tape.headEnableStep);
                else if (tape.headEnableGain[h] > tape.headEnableTarget[h])
                    tape.headEnableGain[h] =
                        std::max(tape.headEnableTarget[h], tape.headEnableGain[h] - tape.headEnableStep);
            }
            const float h1Gain = tape.headEnableGain[0];
            const float h2Gain = tape.headEnableGain[1];
            const float h3Gain = tape.headEnableGain[2];
            const std::array<float, 3> audibleHeadGains{h1Gain, h2Gain, h3Gain};
            const std::array<float, 3> audibleHeadSamples{ynPH1, ynPH2, ynPH3};

            const float audibleKillGain = tape.killActive ? tape.killEnvelope : 1.0f;
            float output = 0.0f;
            for (int head = 0; head < 3; ++head)
            {
                const float treated = tape.processAudibleHeadSample(audibleHeadSamples[static_cast<size_t>(head)] *
                                                                        audibleHeadGains[static_cast<size_t>(head)],
                                                                    head, 0, audibleKillGain, tape.DelayEnabledGain);
                if (tape.perHeadCaptureEnabled)
                    tape.storePerHeadCaptureSample(head, 0, sample, treated);
                output += treated;
            }

            buffer[0][static_cast<size_t>(sample)] = output;

            if (tape.killActive)
            {
                tape.killEnvelope -= tape.killStep;
                if (tape.killEnvelope <= 0.0f)
                {
                    tape.killEnvelope = 0.0f;
                    tape.killActive = false;
                    tape.clearBuffers();
                }
            }

            float gatedInput = xn * tape.EchoSendGain;
            if (tape.killActive)
                gatedInput *= tape.killEnvelope;

            tape.TapeBuffer[0]->BufferWrite(gatedInput);
        }
    }
}

#endif
