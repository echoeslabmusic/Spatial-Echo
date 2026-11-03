// HeadlessTestRunner: The test registry (every headless test, its kind and its entry point) and the helpers the
//   runner executable uses to find and run an entry.

#pragma once

#include <JuceHeader.h>

#include <vector>

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::Headless
{
// test:        default suite; must pass on every supported build.
// measurement: probes, renders, benchmarks and profiles. Run only by name;
//              they print numbers and write their outputs to the artifact
//              directory.
// fixture:     needs an external input (a job spec, a dataset); run only by
//              name with that input supplied.
enum class TestKind
{
    test,
    measurement,
    fixture
};

using ProcessorTest = juce::String (*)(SpatialEchoAudioProcessor&);
using StandaloneTest = juce::String (*)();

// Exactly one of withProcessor and standalone is set. Processor tests get a
// processor prepared at 48 kHz / 512 samples; standalone tests build what
// they need themselves.
struct TestEntry
{
    const char* name = nullptr;
    TestKind kind = TestKind::test;
    ProcessorTest withProcessor = nullptr;
    StandaloneTest standalone = nullptr;
};

const std::vector<TestEntry>& registry();
const TestEntry* findTest(const juce::String& name);
const char* kindName(TestKind kind);

// Runs one entry and returns its result text. Exceptions become [ERROR].
juce::String runTest(const TestEntry& entry);
}  // namespace SpatialEchoTests::Headless
