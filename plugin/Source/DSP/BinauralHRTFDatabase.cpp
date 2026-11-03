// HRTFDatabase: Loads a SOFA HRIR set and samples it by direction through libmysofa.
// Realtime-safety: no allocation/locks in getHRIR() or sampleSofaHRIR()

#include "BinauralProcessor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include "dependencies/sofa/libmysofa/mysofa.h"

namespace
{
juce::String sofaAttr(MYSOFA_ATTRIBUTE* attr, const char* name)
{
    if (attr == nullptr || name == nullptr)
        return {};
    char* value = mysofa_getAttribute(attr, const_cast<char*>(name));
    return value ? juce::String(value) : juce::String();
}

float wrapAzimuthDegrees(float azDeg)
{
    azDeg = std::fmod(azDeg, 360.0f);
    if (azDeg > 180.0f)
        azDeg -= 360.0f;
    if (azDeg < -180.0f)
        azDeg += 360.0f;
    return azDeg;
}

// Returns false for any file the loader cannot render safely. The caller runs
// it again after resampling and after the coordinate conversion, because both
// rewrite the arrays checked here.
bool preflightSofa(MYSOFA_HRTF* hrtf, double targetSampleRate)
{
    if (!hrtf || !std::isfinite(targetSampleRate))
        return false;

    if (targetSampleRate > 0.0 && targetSampleRate < 8000.0)
        return false;

    const auto maxInt = static_cast<unsigned int>(std::numeric_limits<int>::max());
    if (hrtf->M > maxInt || hrtf->R > maxInt || hrtf->N > maxInt || hrtf->C > maxInt)
        return false;

    const int numSources = static_cast<int>(hrtf->M);
    const int numReceivers = static_cast<int>(hrtf->R);
    const int irLength = static_cast<int>(hrtf->N);

    // At least one measurement and two receivers (extra receivers are
    // ignored), with three coordinates per position.
    if (numSources <= 0 || irLength <= 0 || numReceivers < 2 || hrtf->C != 3)
        return false;

    if (hrtf->DataSamplingRate.elements > 0 && hrtf->DataSamplingRate.values == nullptr)
        return false;

    if (hrtf->DataDelay.elements > 0 && hrtf->DataDelay.values == nullptr)
        return false;

    // DataDelay may be one value, one per receiver, one per measurement, or
    // one per measurement and receiver.
    const unsigned long long totalDelayElements =
        static_cast<unsigned long long>(numSources) * static_cast<unsigned long long>(numReceivers);
    const unsigned long long delayElements = static_cast<unsigned long long>(hrtf->DataDelay.elements);
    if (delayElements > 0ull)
    {
        const bool supportedDelayShape =
            delayElements == 1ull || delayElements == static_cast<unsigned long long>(numReceivers) ||
            delayElements == static_cast<unsigned long long>(numSources) || delayElements == totalDelayElements;
        if (!supportedDelayShape)
            return false;
    }

    if (hrtf->ReceiverPosition.elements > 0 && hrtf->ReceiverPosition.values == nullptr)
        return false;

    if (!hrtf->DataIR.values)
        return false;

    // libmysofa and the coordinate loops below index with signed ints.
    // Check before multiplying: three individually valid dimensions can
    // overflow even an unsigned 64-bit product on malformed input.
    if (hrtf->M > maxInt / 3u || hrtf->R > maxInt / 3u || totalDelayElements > maxInt ||
        static_cast<unsigned long long>(irLength) > maxInt / totalDelayElements)
        return false;
    const unsigned long long expectedIrElements = totalDelayElements * static_cast<unsigned long long>(irLength);

    if (static_cast<unsigned long long>(hrtf->DataIR.elements) < expectedIrElements)
        return false;

    const unsigned int minReceiverElements = static_cast<unsigned int>(numReceivers * 3);
    if (!hrtf->ReceiverPosition.values || hrtf->ReceiverPosition.elements < minReceiverElements)
        return false;

    if (!hrtf->SourcePosition.values || hrtf->SourcePosition.elements < static_cast<unsigned int>(numSources * 3))
        return false;

    // These arrays are consumed by libmysofa before the audio-path guards
    // can run. In particular, an infinite source azimuth would stall
    // angle wrapping forever, and non-finite FIR/delay data poison the filter.
    const std::array<const MYSOFA_ARRAY*, 9> arrays{
        {&hrtf->SourcePosition, &hrtf->ReceiverPosition, &hrtf->ListenerPosition, &hrtf->ListenerView,
         &hrtf->ListenerUp, &hrtf->EmitterPosition, &hrtf->DataIR, &hrtf->DataDelay, &hrtf->DataSamplingRate}};
    for (const auto* array : arrays)
    {
        if (array->elements > maxInt || (array->elements > 0 && array->values == nullptr))
            return false;
        for (unsigned int i = 0; i < array->elements; ++i)
        {
            if (!std::isfinite(array->values[i]))
                return false;
        }
    }

    if (hrtf->DataSamplingRate.elements > 1)
        return false;

    const juce::String sofaConv = sofaAttr(hrtf->attributes, "SOFAConventions");
    if (sofaConv.isNotEmpty() && sofaConv != "SimpleFreeFieldHRIR")
        return false;

    const juce::String dataType = sofaAttr(hrtf->attributes, "DataType");
    if (dataType.isNotEmpty() && dataType != "FIR")
        return false;

    // A missing DataSamplingRate is accepted when the target rate can stand in.
    const float sofaSampleRate = (hrtf->DataSamplingRate.values && hrtf->DataSamplingRate.elements > 0)
                                     ? hrtf->DataSamplingRate.values[0]
                                     : 0.0f;
    return sofaSampleRate > 0.0f || targetSampleRate > 0.0;
}

BinauralProcessor::SofaDataDelayShape classifyDataDelayShape(int delayElements, int numSources, int numReceivers)
{
    if (delayElements <= 0)
        return BinauralProcessor::SofaDataDelayShape::Missing;
    if (delayElements == 1)
        return BinauralProcessor::SofaDataDelayShape::Scalar;
    if (delayElements == numReceivers)
        return BinauralProcessor::SofaDataDelayShape::PerReceiver;
    if (delayElements == numSources)
        return BinauralProcessor::SofaDataDelayShape::PerSource;
    if (delayElements == numSources * numReceivers)
        return BinauralProcessor::SofaDataDelayShape::PerSourcePerReceiver;
    return BinauralProcessor::SofaDataDelayShape::Unsupported;
}

void summarizeDelayValues(const float* values, int count, int& nonZeroCount, int& nonFiniteCount, float& minSeconds,
                          float& maxSeconds)
{
    nonZeroCount = 0;
    nonFiniteCount = 0;
    minSeconds = 0.0f;
    maxSeconds = 0.0f;

    if (values == nullptr || count <= 0)
        return;

    bool sawFinite = false;
    for (int i = 0; i < count; ++i)
    {
        const float value = values[i];
        if (!std::isfinite(value))
        {
            ++nonFiniteCount;
            continue;
        }

        if (std::abs(value) > 1.0e-9f)
            ++nonZeroCount;

        if (!sawFinite)
        {
            minSeconds = value;
            maxSeconds = value;
            sawFinite = true;
        }
        else
        {
            minSeconds = std::min(minSeconds, value);
            maxSeconds = std::max(maxSeconds, value);
        }
    }
}
}  // namespace

//==============================================================================
// HRTFDatabase Implementation
//==============================================================================

BinauralProcessor::HRTFDatabase::HRTFDatabase()
{
}

BinauralProcessor::HRTFDatabase::~HRTFDatabase()
{
    releaseSofaEasy();
}

void BinauralProcessor::HRTFDatabase::setEffectiveHrirLength(int length)
{
    hrirLength = juce::jlimit(1, HRIR_MAX_LENGTH, length);
}

const BinauralProcessor::HRIRData& BinauralProcessor::HRTFDatabase::getHRIRByGridIndex(int gridIndex) const
{
    if (gridIndex < 0 || gridIndex >= static_cast<int>(gridPoints.size()) ||
        gridIndex >= static_cast<int>(gridHRIRs.size()))
        return emptyHRIR;

    return gridHRIRs[static_cast<size_t>(gridIndex)];
}

int BinauralProcessor::HRTFDatabase::findNearestGridIndex(float azimuth, float elevation) const
{
    if (gridPoints.empty())
        return -1;

    const float degToRad = juce::MathConstants<float>::pi / 180.0f;
    float azRad = azimuth * degToRad;
    float elRad = elevation * degToRad;
    float tx = std::cos(elRad) * std::sin(azRad);
    float ty = std::cos(elRad) * std::cos(azRad);
    float tz = std::sin(elRad);

    float bestDot = -2.0f;
    int bestIdx = 0;
    for (int i = 0; i < static_cast<int>(gridPoints.size()); ++i)
    {
        const auto& p = gridPoints[static_cast<size_t>(i)];
        float dot = tx * p.x + ty * p.y + tz * p.z;
        if (dot > bestDot)
        {
            bestDot = dot;
            bestIdx = i;
        }
    }

    return bestIdx;
}

BinauralProcessor::HRIRData BinauralProcessor::HRTFDatabase::getHRIR(float azimuth, float elevation) const
{
    const int nearest = dataLoaded ? findNearestGridIndex(azimuth, elevation) : -1;
    if (nearest < 0)
        return HRIRData{};

    return getHRIRByGridIndex(nearest);
}

void BinauralProcessor::HRTFDatabase::releaseSofaEasy()
{
    if (sofaEasy)
    {
        mysofa_close(sofaEasy);
        sofaEasy = nullptr;
    }
    sofaTempL.clear();
    sofaTempR.clear();
    sofaDelayL.clear();
    sofaDelayR.clear();
    sofaFullLength = 0;
    sofaSourceRadius = 1.0f;
}

bool BinauralProcessor::HRTFDatabase::initSofaEasyFromHrtf(MYSOFA_HRTF* hrtf, float neighborAngleStep)
{
    if (!hrtf)
        return false;

    // Prepare HRTF for lookup/interpolation (libmysofa expects cartesian coords).
    mysofa_tocartesian(hrtf);

    auto* easy = static_cast<MYSOFA_EASY*>(std::malloc(sizeof(MYSOFA_EASY)));
    if (!easy)
        return false;

    *easy = MYSOFA_EASY{};

    easy->lookup = mysofa_lookup_init(hrtf);
    if (!easy->lookup)
    {
        mysofa_close(easy);
        return false;
    }

    if (!(neighborAngleStep > 0.0f))
        neighborAngleStep = MYSOFA_DEFAULT_NEIGH_STEP_ANGLE;
    easy->neighborhood = mysofa_neighborhood_init_withstepdefine(hrtf, easy->lookup, neighborAngleStep,
                                                                 MYSOFA_DEFAULT_NEIGH_STEP_RADIUS);
    if (!easy->neighborhood)
    {
        mysofa_close(easy);
        return false;
    }

    easy->fir =
        static_cast<float*>(std::malloc(static_cast<size_t>(hrtf->N) * static_cast<size_t>(hrtf->R) * sizeof(float)));
    if (!easy->fir)
    {
        mysofa_close(easy);
        return false;
    }

    easy->hrtf = hrtf;
    sofaEasy = easy;
    sofaFullLength = static_cast<int>(hrtf->N);
    sofaTempL.assign(static_cast<size_t>(sofaFullLength), 0.0f);
    sofaTempR.assign(static_cast<size_t>(sofaFullLength), 0.0f);
    return true;
}

bool BinauralProcessor::HRTFDatabase::getSofaDelays(int gridIndex, float& delayL, float& delayR) const
{
    if (gridIndex < 0)
        return false;
    const size_t idx = static_cast<size_t>(gridIndex);
    if (idx >= sofaDelayL.size() || idx >= sofaDelayR.size())
        return false;

    delayL = sofaDelayL[idx];
    delayR = sofaDelayR[idx];
    return true;
}

bool BinauralProcessor::HRTFDatabase::sampleSofaHRIR(float azimuth, float elevation, bool interpolate, HRIRData& output,
                                                     float& delayL, float& delayR)
{
    if (!sofaEasy || !sofaEasy->hrtf || sofaFullLength <= 0)
        return false;

    if (sofaTempL.size() != static_cast<size_t>(sofaFullLength))
    {
        sofaTempL.assign(static_cast<size_t>(sofaFullLength), 0.0f);
        sofaTempR.assign(static_cast<size_t>(sofaFullLength), 0.0f);
    }

    const float radius = (sofaSourceRadius > 0.0f) ? sofaSourceRadius : 1.0f;
    float coords[3] = {wrapAzimuthDegrees(-azimuth), elevation, radius};
    mysofa_s2c(coords);

    float dL = 0.0f;
    float dR = 0.0f;
    if (interpolate)
        mysofa_getfilter_float(sofaEasy, coords[0], coords[1], coords[2], sofaTempL.data(), sofaTempR.data(), &dL, &dR);
    else
        mysofa_getfilter_float_nointerp(sofaEasy, coords[0], coords[1], coords[2], sofaTempL.data(), sofaTempR.data(),
                                        &dL, &dR);

    delayL = dL;
    delayR = dR;

    std::fill(output.leftEar.begin(), output.leftEar.end(), 0.0f);
    std::fill(output.rightEar.begin(), output.rightEar.end(), 0.0f);

    const int copyLen = juce::jlimit(1, HRIR_MAX_LENGTH, hrirLength);
    const int sofaCopyLen = juce::jmin(copyLen, sofaFullLength);
    for (int i = 0; i < sofaCopyLen; ++i)
    {
        output.leftEar[static_cast<size_t>(i)] = sofaTempL[static_cast<size_t>(i)];
        output.rightEar[static_cast<size_t>(i)] = sofaTempR[static_cast<size_t>(i)];
    }

    return true;
}

bool BinauralProcessor::HRTFDatabase::loadSOFAFromHrtf(MYSOFA_HRTF* hrtf, double targetSampleRate)
{
    gridPoints.clear();
    gridHRIRs.clear();
    dataLoaded = false;
    sofaConventionReport = {};
    releaseSofaEasy();

    if (!hrtf)
        return false;

    if (!preflightSofa(hrtf, targetSampleRate))
    {
        mysofa_free(hrtf);
        return false;
    }

    int err = mysofa_check(hrtf);
    if (err != MYSOFA_OK)
    {
        mysofa_free(hrtf);
        return false;
    }

    if (targetSampleRate > 0.0 && hrtf->DataSamplingRate.values != nullptr && hrtf->DataSamplingRate.elements > 0 &&
        hrtf->DataSamplingRate.values[0] > 0.0f)
    {
        err = mysofa_resample(hrtf, static_cast<float>(targetSampleRate));
        if (err != MYSOFA_OK)
        {
            mysofa_free(hrtf);
            return false;
        }
    }

    if (!preflightSofa(hrtf, targetSampleRate))
    {
        mysofa_free(hrtf);
        return false;
    }

    mysofa_tospherical(hrtf);
    // Finite Cartesian coordinates can still overflow libmysofa's float
    // radius calculation. Do not feed conversion-produced infinities into
    // our grid or the spatial lookup built below.
    if (!preflightSofa(hrtf, targetSampleRate))
    {
        mysofa_free(hrtf);
        return false;
    }

    const int numSources = static_cast<int>(hrtf->M);
    const int numReceivers = static_cast<int>(hrtf->R);
    const int irLength = static_cast<int>(hrtf->N);

    if (numSources <= 0 || numReceivers < 2 || irLength <= 0)
    {
        mysofa_free(hrtf);
        return false;
    }

    hrirLength = juce::jlimit(1, HRIR_MAX_LENGTH, irLength);

    int leftReceiverIndex = 0;
    int rightReceiverIndex = 1;
    bool receiverSwapApplied = false;
    if (numReceivers >= 2 && hrtf->ReceiverPosition.values && hrtf->ReceiverPosition.elements >= 6)
    {
        const float az0 = wrapAzimuthDegrees(hrtf->ReceiverPosition.values[0]);
        const float az1 = wrapAzimuthDegrees(hrtf->ReceiverPosition.values[3]);
        if (az0 < az1)
        {
            leftReceiverIndex = 1;
            rightReceiverIndex = 0;
            receiverSwapApplied = true;
        }
    }

    const size_t totalDelays = static_cast<size_t>(numSources) * static_cast<size_t>(numReceivers);
    if (totalDelays == 0 || totalDelays > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        totalDelays > static_cast<size_t>(std::numeric_limits<unsigned int>::max()))
    {
        mysofa_free(hrtf);
        return false;
    }
    const int totalDelaysInt = static_cast<int>(totalDelays);
    const int delayElementsRaw = static_cast<int>(hrtf->DataDelay.elements);

    // Normalize DataDelay to one value per (measurement, receiver) so libmysofa interpolation
    // never dereferences missing/undersized delay arrays.
    std::vector<float> normalizedDelays(totalDelays, 0.0f);
    if (hrtf->DataDelay.values && hrtf->DataDelay.elements > 0)
    {
        const int delayElements = static_cast<int>(hrtf->DataDelay.elements);
        if (delayElements >= totalDelaysInt)
        {
            std::copy_n(hrtf->DataDelay.values, totalDelays, normalizedDelays.begin());
        }
        else if (delayElements == numSources)
        {
            for (int m = 0; m < numSources; ++m)
            {
                const float delay = hrtf->DataDelay.values[m];
                const size_t base = static_cast<size_t>(m) * static_cast<size_t>(numReceivers);
                for (int r = 0; r < numReceivers; ++r)
                    normalizedDelays[base + static_cast<size_t>(r)] = delay;
            }
        }
        else if (delayElements >= numReceivers)
        {
            for (int m = 0; m < numSources; ++m)
            {
                const size_t base = static_cast<size_t>(m) * static_cast<size_t>(numReceivers);
                for (int r = 0; r < numReceivers; ++r)
                    normalizedDelays[base + static_cast<size_t>(r)] = hrtf->DataDelay.values[r];
            }
        }
        else if (delayElements == 1)
        {
            std::fill(normalizedDelays.begin(), normalizedDelays.end(), hrtf->DataDelay.values[0]);
        }
    }

    int rawDelayNonZeroCount = 0;
    int rawDelayNonFiniteCount = 0;
    float rawDelayMinSeconds = 0.0f;
    float rawDelayMaxSeconds = 0.0f;
    summarizeDelayValues(hrtf->DataDelay.values, delayElementsRaw, rawDelayNonZeroCount, rawDelayNonFiniteCount,
                         rawDelayMinSeconds, rawDelayMaxSeconds);

    float* delayValues = static_cast<float*>(std::malloc(totalDelays * sizeof(float)));
    if (!delayValues)
    {
        mysofa_free(hrtf);
        return false;
    }
    std::copy(normalizedDelays.begin(), normalizedDelays.end(), delayValues);
    if (hrtf->DataDelay.values)
        std::free(hrtf->DataDelay.values);
    hrtf->DataDelay.values = delayValues;
    hrtf->DataDelay.elements = static_cast<unsigned int>(totalDelays);

    sofaConventionReport.usingSofa = true;
    sofaConventionReport.numSources = numSources;
    sofaConventionReport.numReceivers = numReceivers;
    sofaConventionReport.irLength = irLength;
    sofaConventionReport.leftReceiverIndex = leftReceiverIndex;
    sofaConventionReport.rightReceiverIndex = rightReceiverIndex;
    sofaConventionReport.receiverSwapApplied = receiverSwapApplied;
    sofaConventionReport.azimuthSignFlipped = true;
    sofaConventionReport.dataDelayShape = classifyDataDelayShape(delayElementsRaw, numSources, numReceivers);
    sofaConventionReport.dataDelayElements = delayElementsRaw;
    sofaConventionReport.dataDelayNonZeroCount = rawDelayNonZeroCount;
    sofaConventionReport.dataDelayNonFiniteCount = rawDelayNonFiniteCount;
    sofaConventionReport.dataDelayMinSeconds = rawDelayMinSeconds;
    sofaConventionReport.dataDelayMaxSeconds = rawDelayMaxSeconds;
    sofaConventionReport.normalizedDataDelayShape = BinauralProcessor::SofaDataDelayShape::PerSourcePerReceiver;
    sofaConventionReport.normalizedDataDelayElements = totalDelaysInt;
    summarizeDelayValues(normalizedDelays.data(), totalDelaysInt, sofaConventionReport.normalizedDataDelayNonZeroCount,
                         sofaConventionReport.normalizedDataDelayNonFiniteCount,
                         sofaConventionReport.normalizedDataDelayMinSeconds,
                         sofaConventionReport.normalizedDataDelayMaxSeconds);

    gridPoints.reserve(static_cast<size_t>(numSources));
    gridHRIRs.resize(static_cast<size_t>(numSources));

    sofaSourceRadius = 1.0f;
    if (hrtf->SourcePosition.values)
    {
        double radiusSum = 0.0;
        int radiusCount = 0;
        for (int m = 0; m < numSources; ++m)
        {
            float r = hrtf->SourcePosition.values[m * 3 + 2];
            if (r > 0.0f)
            {
                radiusSum += static_cast<double>(r);
                radiusCount++;
            }
        }
        if (radiusCount > 0)
            sofaSourceRadius = static_cast<float>(radiusSum / static_cast<double>(radiusCount));
    }

    sofaDelayL.assign(static_cast<size_t>(numSources), 0.0f);
    sofaDelayR.assign(static_cast<size_t>(numSources), 0.0f);
    if (hrtf->DataDelay.values && hrtf->DataDelay.elements > 0)
    {
        const int delayElements = static_cast<int>(hrtf->DataDelay.elements);
        if (delayElements >= numSources * numReceivers)
        {
            for (int m = 0; m < numSources; ++m)
            {
                const int base = m * numReceivers;
                sofaDelayL[static_cast<size_t>(m)] = hrtf->DataDelay.values[base + leftReceiverIndex];
                sofaDelayR[static_cast<size_t>(m)] = hrtf->DataDelay.values[base + rightReceiverIndex];
            }
        }
        else if (delayElements >= numReceivers)
        {
            const float delayL = hrtf->DataDelay.values[leftReceiverIndex];
            const float delayR = hrtf->DataDelay.values[rightReceiverIndex];
            for (int m = 0; m < numSources; ++m)
            {
                sofaDelayL[static_cast<size_t>(m)] = delayL;
                sofaDelayR[static_cast<size_t>(m)] = delayR;
            }
        }
        else if (delayElements >= numSources)
        {
            for (int m = 0; m < numSources; ++m)
            {
                float delay = hrtf->DataDelay.values[m];
                sofaDelayL[static_cast<size_t>(m)] = delay;
                sofaDelayR[static_cast<size_t>(m)] = delay;
            }
        }
        else if (delayElements == 1)
        {
            const float delay = hrtf->DataDelay.values[0];
            for (int m = 0; m < numSources; ++m)
            {
                sofaDelayL[static_cast<size_t>(m)] = delay;
                sofaDelayR[static_cast<size_t>(m)] = delay;
            }
        }
    }

    const float degToRad = juce::MathConstants<float>::pi / 180.0f;
    const int copyLen = hrirLength;
    for (int m = 0; m < numSources; ++m)
    {
        const float rawAz = hrtf->SourcePosition.values[m * 3];
        float az = rawAz;
        float el = hrtf->SourcePosition.values[m * 3 + 1];

        // SOFA convention: azimuth is positive to the left. Our UI expects positive to the right.
        az = wrapAzimuthDegrees(-az);

        GridPoint point;
        point.azimuthDeg = az;
        point.elevationDeg = el;
        float azRad = az * degToRad;
        float elRad = el * degToRad;
        point.x = std::cos(elRad) * std::sin(azRad);
        point.y = std::cos(elRad) * std::cos(azRad);
        point.z = std::sin(elRad);
        gridPoints.push_back(point);
        if (!sofaConventionReport.orientationProbeFound &&
            std::abs(wrapAzimuthDegrees(point.azimuthDeg - rawAz)) >= 1.0f)
        {
            sofaConventionReport.orientationProbeFound = true;
            sofaConventionReport.orientationProbeRawAzimuthDeg = rawAz;
            sofaConventionReport.orientationProbeMappedAzimuthDeg = point.azimuthDeg;
            sofaConventionReport.orientationProbeElevationDeg = point.elevationDeg;
        }

        HRIRData hrir;
        std::fill(hrir.leftEar.begin(), hrir.leftEar.end(), 0.0f);
        std::fill(hrir.rightEar.begin(), hrir.rightEar.end(), 0.0f);

        const size_t baseL =
            (static_cast<size_t>(m) * static_cast<size_t>(numReceivers) + static_cast<size_t>(leftReceiverIndex)) *
            static_cast<size_t>(irLength);
        const size_t baseR =
            (static_cast<size_t>(m) * static_cast<size_t>(numReceivers) + static_cast<size_t>(rightReceiverIndex)) *
            static_cast<size_t>(irLength);
        for (size_t n = 0; n < static_cast<size_t>(copyLen); ++n)
        {
            hrir.leftEar[n] = hrtf->DataIR.values[baseL + n];
            hrir.rightEar[n] = hrtf->DataIR.values[baseR + n];
        }

        gridHRIRs[static_cast<size_t>(m)] = hrir;
    }

    float neighborAngleStep = MYSOFA_DEFAULT_NEIGH_STEP_ANGLE;
    if (gridPoints.size() >= 2)
    {
        float minAngle = juce::MathConstants<float>::pi;
        for (size_t i = 0; i < gridPoints.size(); ++i)
        {
            const auto& a = gridPoints[i];
            for (size_t j = i + 1; j < gridPoints.size(); ++j)
            {
                const auto& b = gridPoints[j];
                float dot = a.x * b.x + a.y * b.y + a.z * b.z;
                dot = juce::jlimit(-1.0f, 1.0f, dot);
                const float angle = std::acos(dot);
                if (angle > 1.0e-5f && angle < minAngle)
                    minAngle = angle;
            }
        }
        const float minDeg = minAngle * (180.0f / juce::MathConstants<float>::pi);
        if (minDeg > 0.0f && minDeg < 90.0f)
            neighborAngleStep = juce::jlimit(0.5f, 15.0f, minDeg * 0.9f);
    }

    // Without the libmysofa lookup, getHRIR() still serves the nearest
    // measured direction from the copied grid.
    if (!initSofaEasyFromHrtf(hrtf, neighborAngleStep))
        mysofa_free(hrtf);

    dataLoaded = true;
    sofaConventionReport.gridPoints = static_cast<int>(gridPoints.size());
    return true;
}

bool BinauralProcessor::HRTFDatabase::loadSOFA(const juce::File& sofaFile, double targetSampleRate)
{
    if (!sofaFile.existsAsFile())
        return false;

    int err = 0;
    auto* hrtf = mysofa_load(sofaFile.getFullPathName().toRawUTF8(), &err);
    if (!hrtf || err != MYSOFA_OK)
    {
        if (hrtf)
            mysofa_free(hrtf);
        return false;
    }

    return loadSOFAFromHrtf(hrtf, targetSampleRate);
}

bool BinauralProcessor::HRTFDatabase::loadSOFAFromMemory(const void* data, size_t dataSize, double targetSampleRate)
{
    if (data == nullptr || dataSize == 0)
        return false;

    int err = 0;
    auto* hrtf = mysofa_load_data(reinterpret_cast<const char*>(data), dataSize, &err);
    if (!hrtf || err != MYSOFA_OK)
    {
        if (hrtf)
            mysofa_free(hrtf);
        return false;
    }

    return loadSOFAFromHrtf(hrtf, targetSampleRate);
}
