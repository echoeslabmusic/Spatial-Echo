# Build and Test Guide

Spatial Echo is a CMake-only project for macOS and Windows. Linux is not
supported: there is no Linux preset, and the plug-in is not built or checked
there. The shell wrappers under `scripts/` are written for macOS; the test
scripts also run in Git Bash on Windows. On Windows, build with the
`windows-release` and `windows-release-headless` presets from an x64 MSVC
developer shell. Build identities, presets, and formats are defined in
`CMakeLists.txt`, `CMakePresets.json`, and the modules under `cmake/`.

## What each command touches

| Command family | Filesystem and signing effects |
|---|---|
| Raw `cmake --preset` | Configures under `build/cmake/`; does not install plug-ins. |
| Raw `cmake --build --preset` | Writes build artifacts only. Generated Xcode targets ad-hoc sign build-local bundles. |
| `scripts/build.sh` (any config/target) | Builds only; writes nothing outside `build/`. |
| `scripts/install_macos_plugins.sh` | Replaces the user-level bundle only; no privilege escalation, no signing. |
| `scripts/verify_*` | Read-only checks of installed bundles. |
| `scripts/create_installer_mac.sh` | Stages artifacts and writes packages under `build/release_packages/`; signs and notarizes only when asked to. |

## Prerequisites

- Git and CMake 3.25 or newer.
- macOS: Xcode and the command-line tools. Windows: Visual Studio 2022 Build
  Tools (x64), Ninja, and Inno Setup 6 if you want to build the installer.
- An official JUCE source checkout at JUCE 8.0.12, revision
  `501c07674e1ad693085a7e7c398f205c2677f5da`. CMake uses an existing
  `SPATIAL_ECHO_JUCE_DIR` cache or `-D` value first, then the `JUCE_DIR`
  environment variable, then `~/JUCE`. Configure requires a clean checkout at
  that exact revision and rejects anything else without modifying the
  checkout. Keep a dedicated checkout if your usual JUCE tree has other work
  and select it with `-DSPATIAL_ECHO_JUCE_DIR=/absolute/path/to/JUCE`.
- Python 3 for the preset tooling, the realtime-safety audit, and the render
  metric comparison.

Official upstream JUCE contains `juce_audio_processors_headless`; no fork or
patch is needed.

## CMake presets

| Preset | Generator | Formats | Headless runner | Purpose |
|---|---|---|---|---|
| `macos-debug` | Xcode, host architecture | Standalone | on | Development builds and the Debug test runner (`run_tests.sh --debug`) |
| `macos-universal-release` | Xcode, arm64 + x86_64 | AU, VST3, Standalone | off | Release artifacts |
| `macos-clangd` | Unix Makefiles, host architecture | Standalone | on | `compile_commands.json` for clangd |
| `macos-release-headless` | Xcode, host architecture | Standalone | on | Release test runner (`run_tests.sh` default) |
| `windows-release` | Ninja, x64 | VST3, Standalone | off | Release artifacts |
| `windows-release-headless` | Ninja, x64 | Standalone | on | Release test runner |

Options: `SPATIAL_ECHO_FORMATS` (semicolon list), `SPATIAL_ECHO_BUILD_HEADLESS`
(default on), and `SPATIAL_ECHO_BUILD_HOST_CHECK` (default off; builds
`tools/PluginHostCheck`).

## Raw CMake builds

```bash
# Debug standalone and headless runner
cmake --preset macos-debug
cmake --build --preset macos-debug --target SpatialEcho_Standalone
cmake --build --preset macos-debug --target SpatialEchoHeadlessTests

# Universal Release artifacts
cmake --preset macos-universal-release
cmake --build --preset macos-universal-release --target \
  SpatialEcho_VST3 SpatialEcho_AU SpatialEcho_Standalone

# Release headless runner (tests and measurements)
cmake --preset macos-release-headless
cmake --build --preset macos-release-headless --target SpatialEchoHeadlessTests
```

Windows, from an x64 developer shell:

```bash
cmake --preset windows-release
cmake --build --preset windows-release

cmake --preset windows-release-headless -DSPATIAL_ECHO_BUILD_HOST_CHECK=ON
cmake --build --preset windows-release-headless --target SpatialEchoHeadlessTests SpatialEchoHostCheck
```

Artifact paths depend on the CMake generator. After a build, locate the bundle under the preset's build directory and check its
identity:

```bash
find build/cmake/macos-universal-release -type d \
  \( -name '*.vst3' -o -name '*.component' -o -name '*.app' \) -prune -print

BUNDLE="/absolute/path/to/the/selected/bundle"
plutil -p "$BUNDLE/Contents/Info.plist"
EXECUTABLE="$(plutil -extract CFBundleExecutable raw -o - "$BUNDLE/Contents/Info.plist")"
shasum -a 256 "$BUNDLE/Contents/MacOS/$EXECUTABLE"
```

Compare the product name, bundle identifier, format identity, and executable
name with the values in `CMakeLists.txt`. When you report a test
result, note the bundle path, hash, commit, and whether the tree was dirty.

## Development wrappers (macOS)

```bash
./scripts/build.sh debug standalone
./scripts/build.sh debug headless
./scripts/build.sh release vst3
./scripts/build.sh release au
./scripts/build.sh release all
```

`build.sh` rejects Debug AU/VST3 builds so host scan folders never receive
Debug plug-ins.

## Tests

Every test is one entry in the registry table in
`plugin/Source/Tests/Headless/HeadlessTestRunner.cpp`: a name, a function, and
a kind.

| Kind | Runs | Used for |
|---|---|---|
| `test` | in the default suite | pass/fail contracts; must pass |
| `measurement` | by name only | probes, CPU profiles, baseline renders, the README screenshot |
| `fixture` | by name only | jobs that need an external input, such as `PresetRosterRenders` (reads `SPATIAL_ECHO_ROSTER_SPEC`) |

Test code compiles only into Debug builds and the headless runner; shipped
Release plug-ins contain none of it.

### Running the suite

```bash
./scripts/run_tests.sh             # build the Release runner, run the default suite in parallel
./scripts/run_tests.sh --debug     # the same on the Debug runner, with assertions enabled
```

On an Apple silicon Mac with 8 jobs the Release suite runs in about a minute
and a half after the build, and the Debug suite in about six minutes.

| Option | Effect |
|---|---|
| `-j N` | parallel jobs (default: performance cores) |
| `--tests A,B` / `--test Name` | run only these names, of any kind |
| `--stop-on-failure` | start no new tests after the first failure |
| `--no-build` | use the runner that is already built |
| `--log-path PATH` | where to write the suite log |

The script prints one line per test, then the summary counts, the failures
sorted by name, and the ten slowest tests. Per-test stdout, stderr, exit code,
and time are in the `build/test-run.*` directory named at the end. Durations
are kept in `build/test-durations-<config>.txt` so the next run starts the
slowest tests first.

One test, or the runner directly:

```bash
./scripts/run_test.sh ModeChangeClick
./scripts/run_test.sh ModeChangeClick --debug --no-build

RUNNER=build/cmake/macos-release-headless/Release/Spatial-Echo-HeadlessTests
"$RUNNER" --list       # default-suite names
"$RUNNER" --list-all   # every name and its kind
"$RUNNER" --test ModeChangeClick
```

An unknown name exits with code 2 and an error message. The README screenshot
is the `EditorSnapshot` measurement:

```bash
SPATIAL_ECHO_SNAPSHOT_PATH=docs/images/spatial-echo.png ./scripts/run_test.sh EditorSnapshot
```

### CTest

Configuring a preset with the headless runner registers one CTest test per
default-suite entry; the names are read from `--list` when `ctest` runs.

```bash
cmake --build --preset macos-release-headless --target SpatialEchoHeadlessTests
ctest --preset macos-release-headless -j 8
```

`ctest --preset macos-debug` uses the Debug runner.

### Windows

From Git Bash, after building `windows-release-headless`:

```bash
SPATIAL_ECHO_HEADLESS_RUNNER=build/cmake/windows-release-headless/Spatial-Echo-HeadlessTests.exe \
  ./scripts/run_tests.sh --no-build
```

`ReleaseAccessibility` and `InteractiveWalkthrough` wait for callbacks that
the tests deliver by pumping the macOS run loop, so they do not complete on
Windows; the release workflow leaves them out there.

### Isolation

The suite script gives every test its own artifact directory
(`SPATIAL_ECHO_TEST_ARTIFACT_DIR`) and user-preset library
(`SPATIAL_ECHO_TEST_PRESET_DIR`) inside the run directory, and points
`CFFIXED_USER_HOME` at an empty folder; a test that leaves files there fails as
ERROR. Tests write files only under `SpatialEchoTests::artifactDirectory()`
(`plugin/Source/Tests/Common/TestUtilities.h`). Started without those
variables, the runner makes a private temporary directory and prints its path
when a test left files in it. `SPATIAL_ECHO_HEADLESS_LOG_DIR` moves the run
directory and logs (default `build/`).

The runner always runs in deterministic mode (it sets
`SPATIAL_ECHO_HEADLESS_TEST=1`), so every noise source starts from a fixed
seed.

### Result semantics

Results fail closed. The runner exits 0 only on an explicit `[PASS]` token;
`[FAIL]` maps to exit 1, `[ERROR]`, empty, and untagged output map to exit 2,
and `[SKIP]` maps to exit 3. The suite classifies every run as
pass/fail/error/skip/untagged/assertion with separate summary counts, and it
fails any run whose stderr contains a JUCE assertion unless the test is one of
the buffer-boundary tests that deliberately provoke one (Release builds compile
assertions out, so there those tests report their token like any other). A
`[SKIP]` fails unless the test is on the optional-skip list, which covers
external-fixture preconditions only. Both lists are named arrays
(`OPTIONAL_SKIP_TESTS`, `EXPECTED_ASSERTION_TESTS`) at the top of
`scripts/run_tests.sh`. Parser behaviour is fixture-tested by
`scripts/test_headless_suite_parsing.sh`.

A pass must still carry a nontrivial measurement. A plausible token with a
vacuous measurement proves nothing.

When reporting a failure, include the commit and dirty state, architecture,
configuration, sample rate, prepared and actual callback sizes, channel layout,
exact command, elapsed time, stdout, stderr, and the terminal token. Report a
timeout or forced termination separately from a measured failure.

### Adding a test

1. Define its requirement, stimulus, and failure threshold.
2. Write the function in the test file for its area, declare it in that
   area's header, and add one registry entry with its kind.
3. Make invalid units, missing parameters, no-signal stimuli, required skips,
   and unknown names fail closed.
4. Use the `test` kind only when it is deterministic and finishes in seconds;
   long renders and profiles are `measurement`.
5. Write files only under `artifactDirectory()`.
6. Check that the test fails when the behaviour it claims to cover is
   bypassed.

## Release versus Debug measurements

Use Release builds for CPU, memory, latency, aliasing, stability, and
comparative sound measurements. Debug timings are diagnostic only. The Release
headless runner comes from `macos-release-headless` (or
`windows-release-headless`); `run_tests.sh --debug` timings are not valid
performance measurements. Record the commit, configuration, sample rate, callback sizes,
architecture, test or render name, and command with every result, and do not
mix measurements from different binaries under one conclusion.

Bit-exact render comparisons need deterministic mode: the headless runner
always uses it, and other Debug builds need `SPATIAL_ECHO_DETERMINISTIC=1`;
without it the noise generators are free-running.

## Installing and verifying host plug-ins (macOS)

Build the Release artifacts, then install into the user plug-in folders:

```bash
./scripts/build.sh release all
./scripts/install_macos_plugins.sh --format all [--dry-run]
```

The installer is user-scope only (no privilege escalation), does not sign (the
generated Xcode targets already ad-hoc sign the artifact and the copy is
proven byte-identical), refuses to install while a system-scope copy of the
same bundle exists, and reads the installed bundle back against the source
artifact (executable hash plus `Info.plist` identifier, executable name, and
version) before reporting success.

Verification:

```bash
./scripts/verify_macos_plugin_installs.sh --format all --require-user
```

Development installs never write to the system plug-in folders; that scope
belongs to the installer package. The verifier rejects diverging user/system
duplicates. Bundle names come from `scripts/lib/macos_plugin_paths.sh`, and
every check derives a bundle's executable from its own `CFBundleExecutable`,
so verification cannot hash a wrong-identity binary.
`scripts/test_macos_plugin_install_scripts.sh` exercises install, dry-run,
duplicate rejection, tampered readback, and derivation against temporary
plug-in roots.

### Host smoke checks

Record the artifact's commit, package checksum, bundle identifiers, executable
names, and hashes before installing. Rescan or restart the host afterwards;
which binary a DAW loaded is only proven inside the host session.

| Surface | Minimum check |
|---|---|
| AU | Instantiate in an AU host such as Logic, open/close the editor, process audio, automate one control, save and reopen the project. |
| VST3 | Instantiate in a VST3 host such as Ableton Live or Reaper, repeat the AU checks, compare recalled state. |
| Standalone | Select devices, process audio, resize, reopen, confirm the About identity. |

Then cover the product-specific paths:

- factory and user preset audition, save/load, import/export, dirty state, and
  project recall;
- free/synced echo timing, every representative head mode, Ping-Pong, Dub,
  the host-facing Power parameter, and Auto-Panic;
- Spring/Cloud, every reverb route, engine changes during input and tails;
- Linked/Per-Head placement, every motion pattern, Before/After spatial routes,
  mono/stereo/fold-down;
- Mix, Output, feedback and branch extremes, Output Protection, recovery after
  a watchdog or Power restore;
- minimum/default/larger editor sizes, drawers/scope, keyboard access, screen
  reader names/values/states, popup focus, and non-colour state cues;
- About product/version identity against the artifact.

An issue report needs the artifact hash, machine/OS/architecture/audio device,
host and format, sample rate/callback/channel/tempo, exact preset or Init
state, reproduction steps, expected and observed result, and frequency.

## Validators

### Tracked CMake targets (macOS)

For VST3, make a `pluginval` executable discoverable during configure, then
run the tracked strictness-5 target:

```bash
PATH="/absolute/path/to/pluginval.app/Contents/MacOS:$PATH" \
  cmake --preset macos-universal-release
cmake --build --preset macos-universal-release --target SpatialEchoValidateVST3
```

That target validates the build-tree VST3 bundle and does not install it. It
exists only when CMake finds `pluginval`; the repository does not download
the tool.

For AU, install the Release component into the user folder first, confirm the
installed component's bundle identifier, `CFBundleExecutable`, and executable
hash match the build artifact, then run:

```bash
auval -strict -v aufx SpEc EchL
```

`auval` resolves the registered, installed AU; it does not validate an
arbitrary component path. The tracked `SpatialEchoValidateAU` target runs the
same command and depends on the AU build, but does not install.

### Release validators (both platforms)

`scripts/release_validators.sh` runs pluginval at strictness 10, Steinberg's
VST3 validator, and the plug-in host check against installed or built bundles.
On first use it downloads a pinned pluginval release, checks the archive
against a SHA-256 recorded in the script, and builds the VST3 validator from a
pinned VST3 SDK tag, both into `build/tools/`. The host check (`tools/PluginHostCheck`, CMake option
`SPATIAL_ECHO_BUILD_HOST_CHECK`) loads the AU or VST3 the way a DAW does and
checks reported latency, exact bypass alignment, active-path alignment, bypass
switching clicks, state round trip, bounded output in ragged blocks, and
opening/closing the editor window (it shows a window for about 3 s). A crash
prints a backtrace; the Windows release build carries symbols for it.

```bash
cmake -S . -B build/cmake/hostcheck -DCMAKE_BUILD_TYPE=Release \
  -DSPATIAL_ECHO_FORMATS=Standalone -DSPATIAL_ECHO_BUILD_HEADLESS=OFF \
  -DSPATIAL_ECHO_BUILD_HOST_CHECK=ON
cmake --build build/cmake/hostcheck --target SpatialEchoHostCheck
cp build/cmake/hostcheck/SpatialEchoHostCheck_artefacts/Release/SpatialEchoHostCheck build/
./scripts/release_validators.sh macos build/reports \
  "$HOME/Library/Audio/Plug-Ins/VST3/Spatial-Echo.vst3" \
  "$HOME/Library/Audio/Plug-Ins/Components/Spatial-Echo.component"
```

On Windows pass `windows` and the built VST3 path; the host check binary is
`build/SpatialEchoHostCheck.exe` (override with `SPATIAL_ECHO_HOST_CHECK`).

### Cross-platform render comparison

Cross-platform sound comparison uses factory-preset render metrics from the
Release headless runner. The committed macOS reference lives at
`tools/render-reference/preset_metrics_macos_arm64.json` and is stamped with
the `plugin/Source` tree hash; the comparison refuses a reference from a
different source tree. Regenerate the reference on an Apple silicon Mac after
committing any change under `plugin/Source`:

```bash
./scripts/render_metrics.sh \
  build/cmake/macos-release-headless/Release/Spatial-Echo-HeadlessTests \
  tools/render-reference/preset_metrics_macos_arm64.json
python3 scripts/compare_render_metrics.py <reference.json> <candidate.json> [--report out.json]
```

`compare_render_metrics.py --exact` checks that two renders from the same
machine repeat bit-for-bit; without it, levels and spectra are compared within
tolerances calibrated for compiler and CPU differences.

## Installers

### macOS package

```bash
./scripts/create_installer_mac.sh --skip-sign
./scripts/create_installer_mac.sh --skip-build --skip-sign
```

Output goes to `build/release_packages/`: `Spatial-Echo-v<version>-mac.pkg`,
its `.sha256`, and an unsigned delivery ZIP containing the package, checksum,
and a plain-text install guide. `--skip-sign` leaves the build-local ad-hoc
signatures in place. Releases ship unsigned; users open the package through
the per-item "Open Anyway" path.

Signing and notarization are optional and need an Apple Developer ID. Set
`SIGN_APP` (Developer ID Application) and `SIGN_PKG` (Developer ID Installer),
run without `--skip-sign`, and add `--notarize` with a `notarytool` keychain
profile in `NOTARY_PROFILE` (default `AC_PASSWORD`) to notarize and staple.
`scripts/test_create_installer_mac.sh` builds fixture packages and checks that
each release component replaces an installed bundle even when that bundle
reports a higher version.

### Windows installer

`installer/windows/Spatial-Echo.iss` is an Inno Setup script. The release
workflow compiles it with `ISCC "/DMyAppVersion=<version>"`, installs the
result silently on the runner, and checks the installed VST3 and Standalone
paths with the host check. The Windows installer is also unsigned
(SmartScreen "More info, Run anyway").

## Factory presets

Factory preset values are generated, never hand-edited.
`tools/presets/roster-values.json` is the source of truth;
`python3 tools/presets/gen_factory.py` regenerates
`plugin/Source/Presets/FactoryPresetLibrary.h`, and `--check` verifies the
committed header without writing. The generator refuses to emit when domain
validation (`validate_values.py`), category Mix floors, name uniqueness, or
the load-time story checks (`check_story.py`) fail. See the
[preset system reference](preset-system.md).

## Script-only checks

These do not compile or install the application:

```bash
python3 tools/presets/gen_factory.py --check
./scripts/audit_rt_safety.sh
./scripts/test_headless_suite_parsing.sh
./scripts/test_macos_plugin_install_scripts.sh
```

`audit_rt_safety.sh` is a lexical scan of `processBlock` and the DSP
hot paths, including everything under `plugin/Source/DSP/dependencies/`, for
allocation, locks, blocking waits, I/O, and logging; it catches obvious hazards
but is not proof of every call path.

## Continuous integration

`.github/workflows/ci.yml` has two jobs. `checks` runs on Ubuntu on push to
`main`, on pull requests, and on manual dispatch: C++ formatting
(`scripts/format_sources.sh --check` with clang-format 23.1.0), the generated
factory preset header (`python3 tools/presets/gen_factory.py --check`), the
realtime-safety audit (`scripts/audit_rt_safety.sh`), and the suite script's
parser fixtures (`scripts/test_headless_suite_parsing.sh`). `tests` runs on
pull requests and manual dispatch only, never on push: it builds the Release
headless runner on macOS 15 against the pinned JUCE revision and runs
`scripts/run_tests.sh --no-build`, uploading the logs when it fails.

`.github/workflows/release.yml` runs only on manual dispatch. `windows-check`
builds the Windows VST3, Standalone, headless runner, and host check, runs the
18 release-contract tests (the set verified on Windows), compares
factory-preset render metrics with the committed macOS reference (after
checking that two renders repeat exactly), and runs the validators. `release`
adds the macOS universal build and package, user-scope install plus strict
`auval`, validators on Apple Silicon and Intel runners, the Windows installer
with a silent install check, and then creates a draft GitHub Release (tag
`v<version>` from `CMakeLists.txt`) with both installers and their SHA-256
files attached. Publishing the draft is a manual step.
