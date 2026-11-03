// Spatial-Echo-HeadlessTests: command-line runner for the headless test registry.
//
//   Spatial-Echo-HeadlessTests --list          default-suite test names, one per line
//   Spatial-Echo-HeadlessTests --list-all      every registered entry as name<TAB>kind
//   Spatial-Echo-HeadlessTests --test <Name>   run one entry (a bare name or SPATIAL_ECHO_RUN_TEST also work)
//
// Results fail closed: exit 0 needs an explicit [PASS] token. [FAIL] exits 1,
// [SKIP] exits 3, and [ERROR], untagged output or an unknown name exit 2.
//
// A run writes only inside its artifact directory, SPATIAL_ECHO_TEST_ARTIFACT_DIR.
// When the caller does not pass one, the runner creates a temporary directory,
// prints its path if the test left files there, and removes it otherwise. The
// user preset library defaults to a "presets" folder inside it
// (SPATIAL_ECHO_TEST_PRESET_DIR), so no run touches the real preset folder.

#include <JuceHeader.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <system_error>

#include "Tests/Headless/HeadlessTestRunner.h"

namespace
{
using SpatialEchoTests::Headless::TestKind;

constexpr const char* kArtifactDirVariable = "SPATIAL_ECHO_TEST_ARTIFACT_DIR";
constexpr const char* kPresetDirVariable = "SPATIAL_ECHO_TEST_PRESET_DIR";

int exitCodeForResult(const juce::String& result)
{
    if (result.isEmpty() || result.containsIgnoreCase("[ERROR]"))
        return 2;
    if (result.containsIgnoreCase("[FAIL]"))
        return 1;
    if (result.containsIgnoreCase("[SKIP]"))
        return 3;
    if (result.containsIgnoreCase("[PASS]"))
        return 0;
    return 2;
}

juce::String environmentVariable(const char* name)
{
    if (const char* value = std::getenv(name))
        return juce::String::fromUTF8(value).trim();
    return {};
}

void setEnvironmentVariable(const char* name, const juce::String& value)
{
#if JUCE_WINDOWS
    _putenv_s(name, value.toRawUTF8());
#else
    setenv(name, value.toRawUTF8(), 1);
#endif
}

// std::filesystem honours TMPDIR (and TMP/TEMP on Windows); JUCE's
// tempDirectory on macOS lives under ~/Library/Caches instead.
juce::File createPrivateTempDirectory(const juce::String& testName)
{
    std::error_code error;
    const auto systemTemp = std::filesystem::temp_directory_path(error);
    const juce::File parent(juce::String(error ? std::string("/tmp") : systemTemp.string()));
    const auto suffix = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64());
    auto directory = parent.getChildFile("spatial-echo-" + testName + "-" + suffix);
    return directory.createDirectory() ? directory : juce::File();
}

juce::String validateRegistry()
{
    std::set<std::string> names;
    for (const auto& entry : SpatialEchoTests::Headless::registry())
    {
        if (entry.name == nullptr || (entry.withProcessor == nullptr) == (entry.standalone == nullptr))
            return "[ERROR] Malformed registry entry " + juce::String(entry.name != nullptr ? entry.name : "<null>");
        if (!names.insert(entry.name).second)
            return "[ERROR] Duplicate registry entry " + juce::String(entry.name);
    }
    return {};
}

void printUsage()
{
    std::cerr << "Usage: Spatial-Echo-HeadlessTests --test <Name> | --list | --list-all\n"
              << "Or set SPATIAL_ECHO_RUN_TEST to a test name.\n";
}
}  // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;

    if (const auto registryError = validateRegistry(); registryError.isNotEmpty())
    {
        std::cout << registryError << std::endl;
        return 2;
    }

    juce::String testName;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg = argv[i];
        if (arg == "--list" || arg == "--list-all")
        {
            const bool all = arg == "--list-all";
            for (const auto& entry : SpatialEchoTests::Headless::registry())
            {
                if (all)
                    std::cout << entry.name << '\t' << SpatialEchoTests::Headless::kindName(entry.kind) << '\n';
                else if (entry.kind == TestKind::test)
                    std::cout << entry.name << '\n';
            }
            std::cout.flush();
            return 0;
        }
        if (arg == "-h" || arg == "--help")
        {
            printUsage();
            return 0;
        }
        if (arg == "--test" && i + 1 < argc)
            testName = argv[++i];
        else if (!arg.startsWithChar('-') && testName.isEmpty())
            testName = arg;
        else
        {
            std::cerr << "Unknown argument: " << arg << '\n';
            printUsage();
            return 2;
        }
    }

    if (testName.isEmpty())
        testName = environmentVariable("SPATIAL_ECHO_RUN_TEST");

    if (testName.isEmpty())
    {
        printUsage();
        return 2;
    }

    const auto* entry = SpatialEchoTests::Headless::findTest(testName);
    if (entry == nullptr)
    {
        std::cout << "[ERROR] Unknown test: " << testName << " (run with --list-all to see every registered name)"
                  << std::endl;
        return 2;
    }

    // Test builds seed every noise source from fixed values when this is set
    // (DSP/DeterministicTestMode.h); the registered tests rely on that.
    setEnvironmentVariable("SPATIAL_ECHO_HEADLESS_TEST", "1");

    juce::File ownedArtifactDirectory;
    if (environmentVariable(kArtifactDirVariable).isEmpty())
    {
        ownedArtifactDirectory = createPrivateTempDirectory(testName);
        if (ownedArtifactDirectory == juce::File())
        {
            std::cout << "[ERROR] Could not create a temporary artifact directory" << std::endl;
            return 2;
        }
        setEnvironmentVariable(kArtifactDirVariable, ownedArtifactDirectory.getFullPathName());
    }

    const juce::File artifactDirectory(environmentVariable(kArtifactDirVariable));
    artifactDirectory.createDirectory();

    juce::File ownedPresetDirectory;
    if (environmentVariable(kPresetDirVariable).isEmpty())
    {
        ownedPresetDirectory = artifactDirectory.getChildFile("presets");
        setEnvironmentVariable(kPresetDirVariable, ownedPresetDirectory.getFullPathName());
    }

    const auto result = SpatialEchoTests::Headless::runTest(*entry);

    if (ownedPresetDirectory != juce::File())
        ownedPresetDirectory.deleteRecursively();

    if (ownedArtifactDirectory != juce::File())
    {
        // Empty scratch folders do not count as artifacts.
        if (ownedArtifactDirectory.findChildFiles(juce::File::findFiles, true).isEmpty())
            ownedArtifactDirectory.deleteRecursively();
        else
            std::cerr << "Artifacts: " << ownedArtifactDirectory.getFullPathName() << '\n';
    }

    std::cout << result << std::endl;

    // Normal teardown is part of the test: a shutdown fault after the result
    // line still fails the run through the process exit status.
    return exitCodeForResult(result);
}
