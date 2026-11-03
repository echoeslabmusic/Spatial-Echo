// BinauralProcessorSofa: HRTF dataset loading (the embedded default or a SOFA file).

#include "BinauralProcessor.h"
#include "dependencies/sofa/libmysofa/mysofa.h"

bool BinauralProcessor::loadSOFAFile(const juce::File& sofaFile)
{
    if (!hrtfDatabase)
        return false;

    const bool ok = hrtfDatabase->loadSOFA(sofaFile, sampleRate);
    if (ok)
    {
        usingDefaultHrtf = false;
        onHrtfDatabaseChanged();
    }
    else
    {
        useDefaultHRTF();
    }
    return ok;
}

void BinauralProcessor::useDefaultHRTF()
{
    if (!hrtfDatabase)
        return;

    // The bundled dataset is compiled in through juce_add_binary_data, so it
    // never depends on a file next to the binary. isHrtfLoaded() reports the
    // outcome.
    const bool ok = hrtfDatabase->loadSOFAFromMemory(
        BinaryData::pp1_HRIRs_measured_sofa, static_cast<size_t>(BinaryData::pp1_HRIRs_measured_sofaSize), sampleRate);
    if (ok)
    {
        usingDefaultHrtf = true;
        onHrtfDatabaseChanged();
    }
}

bool BinauralProcessor::isHrtfLoaded() const
{
    return hrtfDatabase && hrtfDatabase->isLoaded();
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
bool BinauralProcessor::loadSofaHrtfForTest(MYSOFA_HRTF* hrtf)
{
    if (!hrtfDatabase)
    {
        if (hrtf)
            mysofa_free(hrtf);
        return false;
    }

    const bool ok = hrtfDatabase->loadSOFAFromHrtf(hrtf, sampleRate);
    if (ok)
    {
        usingDefaultHrtf = false;
        onHrtfDatabaseChanged();
    }
    else
    {
        useDefaultHRTF();
    }

    return ok;
}
#endif
