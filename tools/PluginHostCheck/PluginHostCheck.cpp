// PluginHostCheck: Load the built Spatial Echo AU or VST3 through JUCE's plug-in hosting (the path a DAW takes) and
//   check the release contracts no hand test covers: reported latency, exact bypass alignment, active-path alignment,
//   bypass switching clicks, state round trip, ragged blocks, finite, bounded output, and opening/closing the editor
//   window. Writes a JSON report; a crash prints a backtrace.

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <vector>

namespace
{
constexpr int kExpectedLatency = 4;
constexpr int kBlockSize = 512;
const double kSampleRates[] = {44100.0, 48000.0, 96000.0};

struct Check
{
    juce::String name;
    bool pass = false;
    juce::String detail;
};

struct Report
{
    std::vector<Check> checks;

    void add(const juce::String& name, bool pass, const juce::String& detail)
    {
        checks.push_back({name, pass, detail});
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << name << ": " << detail << std::endl;
    }

    bool allPassed() const
    {
        return std::all_of(checks.begin(), checks.end(), [](const Check& c) { return c.pass; });
    }
};

struct Loader
{
    juce::AudioPluginFormatManager formats;
    juce::PluginDescription description;
    bool found = false;

    Loader(const juce::String& formatName, const juce::String& identifier)
    {
        juce::addDefaultFormatsToManager(formats);

        for (auto* format : formats.getFormats())
        {
            if (format->getName() != formatName)
                continue;

            juce::OwnedArray<juce::PluginDescription> types;
            format->findAllTypesForFile(types, identifier);
            if (!types.isEmpty())
            {
                description = *types.getFirst();
                found = true;
            }
        }
    }

    std::unique_ptr<juce::AudioPluginInstance> create(double sampleRate, juce::String& error)
    {
        return formats.createPluginInstance(description, sampleRate, kBlockSize, error);
    }
};

juce::AudioProcessorParameter* findParameter(juce::AudioPluginInstance& plugin, const juce::String& name)
{
    for (auto* parameter : plugin.getParameters())
        if (parameter->getName(128) == name)
            return parameter;
    return nullptr;
}

void prepare(juce::AudioPluginInstance& plugin, double sampleRate, int blockSize)
{
    plugin.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    plugin.prepareToPlay(sampleRate, blockSize);
}

// Runs `input` (stereo, one vector per channel) through the plug-in in blocks
// of `blockSizes` (cycled) and returns the output.
std::vector<std::vector<float>> render(juce::AudioPluginInstance& plugin, const std::vector<std::vector<float>>& input,
                                       const std::vector<int>& blockSizes,
                                       const std::function<void(int)>& beforeBlock = {})
{
    const int total = static_cast<int>(input[0].size());
    std::vector<std::vector<float>> output(2, std::vector<float>(static_cast<size_t>(total), 0.0f));
    const int maxBlock = *std::max_element(blockSizes.begin(), blockSizes.end());
    juce::AudioBuffer<float> buffer(2, maxBlock);
    juce::MidiBuffer midi;

    size_t blockIndex = 0;
    for (int done = 0; done < total;)
    {
        const int n = std::min(blockSizes[blockIndex++ % blockSizes.size()], total - done);
        if (beforeBlock)
            beforeBlock(done);

        buffer.setSize(2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch)
            buffer.copyFrom(ch, 0, input[static_cast<size_t>(ch)].data() + done, n);

        plugin.processBlock(buffer, midi);

        for (int ch = 0; ch < 2; ++ch)
            std::copy_n(buffer.getReadPointer(ch), n, output[static_cast<size_t>(ch)].data() + done);
        done += n;
    }
    return output;
}

std::vector<std::vector<float>> makeSilence(int length)
{
    return std::vector<std::vector<float>>(2, std::vector<float>(static_cast<size_t>(length), 0.0f));
}

std::vector<std::vector<float>> makeImpulse(int length, int position, float amplitude)
{
    std::vector<std::vector<float>> signal(2, std::vector<float>(static_cast<size_t>(length), 0.0f));
    signal[0][static_cast<size_t>(position)] = amplitude;
    signal[1][static_cast<size_t>(position)] = amplitude;
    return signal;
}

std::vector<std::vector<float>> makeNoise(int length, float amplitude, juce::int64 seed)
{
    juce::Random random(seed);
    std::vector<std::vector<float>> signal(2, std::vector<float>(static_cast<size_t>(length)));
    for (auto& channel : signal)
        for (auto& sample : channel)
            sample = amplitude * (random.nextFloat() * 2.0f - 1.0f);
    return signal;
}

std::vector<std::vector<float>> makeSine(int length, double sampleRate, double frequency, float amplitude)
{
    std::vector<std::vector<float>> signal(2, std::vector<float>(static_cast<size_t>(length)));
    for (int i = 0; i < length; ++i)
    {
        const auto v =
            amplitude * static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * frequency * i / sampleRate));
        signal[0][static_cast<size_t>(i)] = v;
        signal[1][static_cast<size_t>(i)] = v;
    }
    return signal;
}

bool allFinite(const std::vector<std::vector<float>>& signal)
{
    for (const auto& channel : signal)
        for (auto sample : channel)
            if (!std::isfinite(sample))
                return false;
    return true;
}

float peakOf(const std::vector<std::vector<float>>& signal)
{
    float peak = 0.0f;
    for (const auto& channel : signal)
        for (auto sample : channel)
            peak = std::max(peak, std::abs(sample));
    return peak;
}

int peakIndex(const std::vector<float>& channel)
{
    return static_cast<int>(
        std::distance(channel.begin(), std::max_element(channel.begin(), channel.end(),
                                                        [](float a, float b) { return std::abs(a) < std::abs(b); })));
}

void checkLatencyAndBypass(Loader& loader, Report& report)
{
    for (const auto sampleRate : kSampleRates)
    {
        const juce::String rate = juce::String(static_cast<int>(sampleRate));
        juce::String error;
        auto plugin = loader.create(sampleRate, error);
        if (plugin == nullptr)
        {
            report.add("load@" + rate, false, error);
            continue;
        }

        prepare(*plugin, sampleRate, kBlockSize);
        const int latency = plugin->getLatencySamples();
        report.add("reported-latency@" + rate, latency == kExpectedLatency,
                   "reported " + juce::String(latency) + ", expected " + juce::String(kExpectedLatency));

        // Host bypass: output must be the input delayed by exactly the reported latency.
        auto* bypass = plugin->getBypassParameter();
        if (bypass == nullptr)
        {
            report.add("bypass-alignment@" + rate, false, "plug-in exposes no bypass parameter to the host");
        }
        else
        {
            bypass->setValueNotifyingHost(1.0f);
            render(*plugin, makeSilence(kBlockSize * 4), {kBlockSize});
            const int position = 1000;
            const auto out = render(*plugin, makeImpulse(kBlockSize * 8, position, 0.5f), {kBlockSize});
            const int at = peakIndex(out[0]);
            float residual = 0.0f;
            for (size_t i = 0; i < out[0].size(); ++i)
                if (static_cast<int>(i) != position + latency)
                    residual = std::max({residual, std::abs(out[0][i]), std::abs(out[1][i])});
            const bool exact = at == position + latency && std::abs(out[0][static_cast<size_t>(at)] - 0.5f) < 1.0e-6f &&
                               residual < 1.0e-6f;
            report.add("bypass-alignment@" + rate, exact,
                       "impulse at " + juce::String(position) + " came out at " + juce::String(at) + ", residual " +
                           juce::String(residual, 8));
            bypass->setValueNotifyingHost(0.0f);
        }

        // Active path with Mix at 0: the dry signal passes the output stage, whose
        // IIR keeps some phase, so the main lobe lands at the reported latency ±1.
        auto* mix = findParameter(*plugin, "Mix");
        if (mix == nullptr)
        {
            report.add("active-alignment@" + rate, false, "no Mix parameter");
            continue;
        }
        plugin->reset();
        mix->setValueNotifyingHost(0.0f);
        render(*plugin, makeSilence(static_cast<int>(sampleRate)), {kBlockSize});
        const int position = 1000;
        const auto out = render(*plugin, makeImpulse(kBlockSize * 8, position, 0.1f), {kBlockSize});
        const int offset = peakIndex(out[0]) - position;
        report.add("active-alignment@" + rate, std::abs(offset - latency) <= 1 && allFinite(out),
                   "dry impulse peak offset " + juce::String(offset) + " samples");
        plugin->releaseResources();
    }
}

void checkBypassSwitching(Loader& loader, Report& report)
{
    const double sampleRate = 48000.0;
    juce::String error;
    auto plugin = loader.create(sampleRate, error);
    if (plugin == nullptr || plugin->getBypassParameter() == nullptr)
    {
        report.add("bypass-switch-clicks", false, plugin == nullptr ? error : "no bypass parameter");
        return;
    }

    prepare(*plugin, sampleRate, kBlockSize);
    auto* bypass = plugin->getBypassParameter();
    const int length = static_cast<int>(sampleRate * 6.0);
    const int togglePeriod = static_cast<int>(sampleRate * 0.5) / kBlockSize * kBlockSize;
    std::vector<int> toggles;

    const auto input = makeSine(length, sampleRate, 220.0, 0.25f);
    const auto out = render(*plugin, input, {kBlockSize},
                            [&](int position)
                            {
                                if (position > 0 && position % togglePeriod == 0)
                                {
                                    bypass->setValueNotifyingHost(bypass->getValue() > 0.5f ? 0.0f : 1.0f);
                                    toggles.push_back(position);
                                }
                            });

    // A click is a jump in the second difference that the signal does not
    // otherwise produce. Compare each switch point with the steady signal
    // between switches.
    auto secondDifference = [&](size_t i)
    {
        float v = 0.0f;
        for (const auto& channel : out)
            v = std::max(v, std::abs(channel[i] - 2.0f * channel[i - 1] + channel[i - 2]));
        return v;
    };

    const int guard = 256;
    float steady = 0.0f;
    float atSwitch = 0.0f;
    for (size_t i = 2; i < out[0].size(); ++i)
    {
        const int position = static_cast<int>(i);
        const bool nearSwitch = std::any_of(toggles.begin(), toggles.end(),
                                            [&](int t) { return position >= t - guard && position < t + guard; });
        (nearSwitch ? atSwitch : steady) = std::max(nearSwitch ? atSwitch : steady, secondDifference(i));
    }

    const float ratio = steady > 0.0f ? atSwitch / steady : 0.0f;
    report.add("bypass-switch-clicks", allFinite(out) && ratio <= 2.0f,
               juce::String(static_cast<int>(toggles.size())) + " switches; switch/steady second-difference ratio " +
                   juce::String(ratio, 3) + " (limit 2.0)");
}

void checkStateRoundTrip(Loader& loader, Report& report)
{
    const double sampleRate = 48000.0;
    juce::String error;
    auto first = loader.create(sampleRate, error);
    auto second = loader.create(sampleRate, error);
    if (first == nullptr || second == nullptr)
    {
        report.add("state-round-trip", false, error);
        return;
    }

    // Move the first instance away from defaults so the round trip carries data.
    if (auto* mix = findParameter(*first, "Mix"))
        mix->setValueNotifyingHost(0.73f);

    prepare(*first, sampleRate, kBlockSize);
    prepare(*second, sampleRate, kBlockSize);
    render(*first, makeSilence(kBlockSize * 4), {kBlockSize});

    juce::MemoryBlock saved;
    first->getStateInformation(saved);
    second->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    render(*second, makeSilence(kBlockSize * 4), {kBlockSize});

    juce::MemoryBlock restored;
    second->getStateInformation(restored);

    int parameterMismatches = 0;
    const auto& firstParameters = first->getParameters();
    const auto& secondParameters = second->getParameters();
    for (int i = 0; i < std::min(firstParameters.size(), secondParameters.size()); ++i)
        if (std::abs(firstParameters[i]->getValue() - secondParameters[i]->getValue()) > 1.0e-6f)
            ++parameterMismatches;
    parameterMismatches += std::abs(firstParameters.size() - secondParameters.size());

    // Release builds seed tape noise and wow/flutter per instance, so two
    // instances never match sample for sample; compare level instead.
    first->reset();
    second->reset();
    const auto input = makeNoise(static_cast<int>(sampleRate * 3.0), 0.2f, 1234);
    const auto a = render(*first, input, {kBlockSize});
    const auto b = render(*second, input, {kBlockSize});
    auto rmsDb = [](const std::vector<float>& channel)
    {
        double sum = 0.0;
        for (auto sample : channel)
            sum += static_cast<double>(sample) * sample;
        return 10.0 * std::log10(std::max(1.0e-20, sum / static_cast<double>(channel.size())));
    };
    double levelDifference = 0.0;
    for (size_t ch = 0; ch < 2; ++ch)
        levelDifference = std::max(levelDifference, std::abs(rmsDb(a[ch]) - rmsDb(b[ch])));

    report.add("state-round-trip", saved == restored && parameterMismatches == 0 && levelDifference <= 0.5,
               "state " + juce::String(static_cast<int>(saved.getSize())) + " bytes, " +
                   (saved == restored ? "identical after restore" : "CHANGED after restore") + "; " +
                   juce::String(parameterMismatches) + " parameter mismatches; level difference " +
                   juce::String(levelDifference, 3) + " dB");
}

// Runs last: an editor crash ends the process, and the crash handler prints
// where it happened.
void checkEditor(Loader& loader, Report& report)
{
    const double sampleRate = 48000.0;
    juce::String error;
    auto plugin = loader.create(sampleRate, error);
    if (plugin == nullptr || !plugin->hasEditor())
    {
        report.add("editor-open-close", false, plugin == nullptr ? error : "plug-in reports no editor");
        return;
    }

    prepare(*plugin, sampleRate, kBlockSize);
    std::cout << "  creating editor" << std::endl;
    std::unique_ptr<juce::AudioProcessorEditor> editor(plugin->createEditorIfNeeded());
    if (editor == nullptr)
    {
        report.add("editor-open-close", false, "createEditorIfNeeded returned null");
        return;
    }

    {
        juce::DocumentWindow window("Spatial Echo host check", juce::Colours::black, 0);
        window.setUsingNativeTitleBar(true);
        window.setContentNonOwned(editor.get(), true);
        window.setVisible(true);
        std::cout << "  editor shown, pumping events for 3 s" << std::endl;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(3000);
        window.setVisible(false);
        window.clearContentComponent();
    }

    const auto bounds = editor->getBounds();
    editor.reset();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    report.add("editor-open-close", true,
               "editor " + juce::String(bounds.getWidth()) + "x" + juce::String(bounds.getHeight()) +
                   " shown for 3 s and closed");
}

void checkRaggedAndBounded(Loader& loader, Report& report)
{
    for (const auto sampleRate : kSampleRates)
    {
        const juce::String rate = juce::String(static_cast<int>(sampleRate));
        juce::String error;
        auto plugin = loader.create(sampleRate, error);
        if (plugin == nullptr)
        {
            report.add("ragged-blocks@" + rate, false, error);
            continue;
        }

        prepare(*plugin, sampleRate, kBlockSize);
        const auto loud = makeNoise(static_cast<int>(sampleRate * 10.0), 0.9f, 99);
        const auto out = render(*plugin, loud, {1, 17, kBlockSize, 333, 64, kBlockSize, 2, 480});
        const float peak = peakOf(out);
        // Output protection holds the output near full scale; allow a small
        // inter-sample overshoot margin.
        report.add("ragged-blocks@" + rate, allFinite(out) && peak <= 1.12f,
                   "10 s near-full-scale noise in blocks 1..512, peak " + juce::String(peak, 4));
    }
}
}  // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::SystemStats::setApplicationCrashHandler(
        [](void*)
        {
            std::cerr << "[CRASH] host check crashed; backtrace:\n"
                      << juce::SystemStats::getStackBacktrace() << std::endl;
            std::cout << "HOST CHECK FAILED (crash)" << std::endl;
            std::fflush(nullptr);
            std::_Exit(3);
        });

    juce::String formatName, identifier, reportPath;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const juce::String key = argv[i];
        const juce::String value = juce::String::fromUTF8(argv[i + 1]);
        if (key == "--format")
            formatName = value;
        else if (key == "--plugin")
            identifier = value;
        else if (key == "--report")
            reportPath = value;
    }

    if (formatName.isEmpty() || identifier.isEmpty())
    {
        std::cerr << "Usage: PluginHostCheck --format VST3|AudioUnit --plugin <path or AU id> [--report out.json]\n"
                     "  AU id example: AudioUnit:Effects/aufx,SpEc,EchL\n";
        return 2;
    }

    Loader loader(formatName, identifier);
    if (!loader.found)
    {
        std::cerr << "[ERROR] no " << formatName << " plug-in found at " << identifier << std::endl;
        return 2;
    }
    std::cout << "Loaded " << loader.description.name << " " << loader.description.version << " ("
              << loader.description.pluginFormatName << ")" << std::endl;

    Report report;
    checkLatencyAndBypass(loader, report);
    checkBypassSwitching(loader, report);
    checkStateRoundTrip(loader, report);
    checkRaggedAndBounded(loader, report);
    checkEditor(loader, report);

    if (reportPath.isNotEmpty())
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("plugin", loader.description.name);
        root->setProperty("version", loader.description.version);
        root->setProperty("format", formatName);
        root->setProperty("identifier", identifier);
        juce::Array<juce::var> rows;
        for (const auto& check : report.checks)
        {
            auto* row = new juce::DynamicObject();
            row->setProperty("name", check.name);
            row->setProperty("pass", check.pass);
            row->setProperty("detail", check.detail);
            rows.add(juce::var(row));
        }
        root->setProperty("checks", rows);
        root->setProperty("pass", report.allPassed());
        juce::File::getCurrentWorkingDirectory()
            .getChildFile(reportPath)
            .replaceWithText(juce::JSON::toString(juce::var(root)));
    }

    std::cout << (report.allPassed() ? "HOST CHECK PASSED" : "HOST CHECK FAILED") << std::endl;
    return report.allPassed() ? 0 : 1;
}
