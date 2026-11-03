// HRTFDatabase: Loads a SOFA HRIR set and samples it by direction through libmysofa.
// Realtime-safety: no allocation/locks in getHRIR() or sampleSofaHRIR()

#pragma once

/**
 * HRTFDatabase - measured HRIR set loaded from a SOFA SimpleFreeFieldHRIR file.
 *
 * Internal module header: included at the end of BinauralProcessor.h.
 */
class BinauralProcessor::HRTFDatabase
{
public:
    HRTFDatabase();
    ~HRTFDatabase();

    // Nearest measured direction; silence when nothing is loaded.
    HRIRData getHRIR(float azimuth, float elevation) const;
    bool isLoaded() const { return dataLoaded; }

    // One measured direction in the plugin's convention (azimuth positive to
    // the right) with its unit vector.
    struct GridPoint
    {
        float azimuthDeg = 0.0f;
        float elevationDeg = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    const std::vector<GridPoint>& getGridPoints() const { return gridPoints; }
    const HRIRData& getHRIRByGridIndex(int gridIndex) const;
    bool loadSOFA(const juce::File& sofaFile, double targetSampleRate);
    bool loadSOFAFromMemory(const void* data, size_t dataSize, double targetSampleRate);
    void setEffectiveHrirLength(int length);
    int getHrirLength() const { return hrirLength; }
    SofaConventionReport getSofaConventionReport() const { return sofaConventionReport; }
    // Samples the dataset at a direction, interpolated by libmysofa or taken
    // from the nearest measurement. Returns false when the lookup is unavailable.
    bool sampleSofaHRIR(float azimuth, float elevation, bool interpolate, HRIRData& output, float& delayL,
                        float& delayR);
    bool getSofaDelays(int gridIndex, float& delayL, float& delayR) const;

private:
    bool dataLoaded = false;
    int hrirLength = HRIR_DEFAULT_LENGTH;
    std::vector<GridPoint> gridPoints;
    std::vector<HRIRData> gridHRIRs;
    HRIRData emptyHRIR{};
    SofaConventionReport sofaConventionReport{};

    bool loadSOFAFromHrtf(MYSOFA_HRTF* hrtf, double targetSampleRate);
    bool initSofaEasyFromHrtf(MYSOFA_HRTF* hrtf, float neighborAngleStep);
    void releaseSofaEasy();
    MYSOFA_EASY* sofaEasy = nullptr;
    int sofaFullLength = 0;
    float sofaSourceRadius = 1.0f;
    std::vector<float> sofaTempL;
    std::vector<float> sofaTempR;
    std::vector<float> sofaDelayL;
    std::vector<float> sofaDelayR;

    int findNearestGridIndex(float azimuth, float elevation) const;
    friend class BinauralProcessor;
};
