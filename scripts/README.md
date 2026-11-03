# Scripts

Build, test, packaging, and release utilities. The shell scripts are written
for macOS (bash + python3). `release_validators.sh`, `render_metrics.sh`,
`run_tests.sh --no-build`, and `run_test.sh --no-build` also run in Git Bash
on Windows; the
rest of the Windows build is driven by the CMake presets directly (see
`docs/build-and-test.md`).

## Build and test

- `build.sh`: build-only macOS entry point,
  `./scripts/build.sh [release|debug] [vst3|au|standalone|headless|all]`.
  Writes nothing outside `build/`; installing is the separate
  `install_macos_plugins.sh` step. Debug AU/VST3 builds are rejected so host
  scan folders never receive Debug plug-ins.
- `run_tests.sh`: runs the default test suite in parallel,
  `./scripts/run_tests.sh [--debug] [-j N] [--tests A,B] [--test Name]
  [--stop-on-failure] [--no-build] [--log-path Path]`. Builds and uses the
  Release headless runner unless `--debug` is given. The test list comes from
  the runner's `--list`. Results fail closed with separate
  pass/fail/error/skip/untagged/assertion counts; the end of the output lists
  failures and the ten slowest tests. Each run gets its own `build/test-run.*`
  directory with per-test logs, artifact directories and preset libraries.
  `SPATIAL_ECHO_HEADLESS_RUNNER` overrides the runner path and
  `SPATIAL_ECHO_HEADLESS_LOG_DIR` the log parent.
- `run_test.sh`: runs one test by name,
  `./scripts/run_test.sh <TestName> [--debug] [--no-build]`. Honors
  `SPATIAL_ECHO_HEADLESS_RUNNER`.
- `test_headless_suite_parsing.sh`: fixtures for the suite's result classifier,
  per-test isolation, parallel scheduling and log handling, run against a fake
  runner.
- `gen_compile_commands.sh`: generates `compile_commands.json` for clangd from
  the `macos-clangd` preset and symlinks it into the repository root.
- `audit_rt_safety.sh`: scans `processBlock` and the DSP hot paths, including
  everything under `plugin/Source/DSP/dependencies/`, for allocation, locks,
  blocking waits, I/O, logging, and other realtime hazards (wraps
  `tools/python_audit/rt_safety_audit.py`).

Raw CMake is equivalent; builds are install-free either way:

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug --target SpatialEcho_Standalone

cmake --preset macos-release-headless
cmake --build --preset macos-release-headless --target SpatialEchoHeadlessTests
```

`run_tests.sh` runs the whole default suite in a few minutes. Performance and
DSP measurements need the Release headless target; Debug timings are not valid
performance measurements.

JUCE is the official upstream source, selected first by an existing
`SPATIAL_ECHO_JUCE_DIR` CMake cache or `-D` value, then by the `JUCE_DIR`
environment variable, then by `~/JUCE`. Configure requires a clean checkout at
JUCE 8.0.12 revision `501c07674e1ad693085a7e7c398f205c2677f5da`.

## Local plug-in install (macOS)

- `install_macos_plugins.sh`: user-scope development install of the built
  Release VST3/AU, `--format all|vst3|au [--dry-run]`. Refuses to install while
  a system copy exists, contains no privilege escalation, does not re-sign (the
  build's ad-hoc signature is preserved), and reads the installed bundle back
  against the source artifact by executable hash and `Info.plist` identity.
- `verify_macos_plugin_installs.sh`: checks that user and system AU/VST3
  installs resolve to matching binaries; `--expect-bundle` proves an install
  matches a source artifact; `--require-user` fails when the user copy is
  missing. Executables are derived from each bundle's `Info.plist`.
- `lib/macos_plugin_paths.sh`: shared bundle identity and paths (single source
  for the installed bundle basename) plus `Info.plist` derivation helpers.
  Sourced, not executed.
- `test_macos_plugin_install_scripts.sh`: temp-root tests for the install and
  verify scripts above; never touches the real plug-in folders.

## Packaging (macOS)

- `create_installer_mac.sh`: builds the universal VST3, AU, and Standalone
  through the tracked CMake preset and assembles the `.pkg` with
  `pkgbuild`/`productbuild`. Flags: `--skip-build`, `--skip-sign`,
  `--notarize`. Output: `build/release_packages/Spatial-Echo-v<version>-mac.pkg`
  plus `.sha256`; an unsigned build also produces
  `...-mac-unsigned.zip` with the installer, checksum, and a plain-text install
  guide. Signing uses `SIGN_APP`/`SIGN_PKG` Developer ID identities and
  notarization uses the `NOTARY_PROFILE` keychain profile. Resources live under
  `installer/`.
- `test_create_installer_mac.sh`: builds fixture component packages and
  verifies that the release package replaces an installed bundle even when
  that bundle reports a higher version, and rejects incomplete artifacts.

The Windows installer is an Inno Setup script, `installer/windows/Spatial-Echo.iss`.

## Release checks (macOS and Windows)

- `release_validators.sh`:
  `./scripts/release_validators.sh macos|windows <report-dir> <vst3> [au]`.
  Runs pluginval at strictness 10, Steinberg's VST3 validator, and the
  `tools/PluginHostCheck` latency/bypass/state/bounds checks. On first use it
  fetches a pinned pluginval release, checks the archive against a recorded
  SHA-256, and builds the VST3 validator from a pinned VST3 SDK tag, both into
  `build/tools/`. The host
  check binary is `build/SpatialEchoHostCheck[.exe]` (override with
  `SPATIAL_ECHO_HOST_CHECK`).
- `render_metrics.sh`: `./scripts/render_metrics.sh <headless-runner> <out.json>
  [wav-dir]`. Renders every factory preset deterministically through the
  Release headless runner and writes per-preset level and spectrum metrics
  stamped with the `plugin/Source` tree hash. The committed macOS reference is
  `tools/render-reference/preset_metrics_macos_arm64.json`.
- `compare_render_metrics.py`: compares two render-metric files within
  calibrated cross-platform tolerances (`--exact` for same-machine repeat
  checks, `--report` to write a JSON report) and refuses a reference from a
  different source tree.

## Preset tooling (`tools/presets/`)

- `gen_factory.py`: generates `plugin/Source/Presets/FactoryPresetLibrary.h`
  from `roster-values.json`: `python3 tools/presets/gen_factory.py`; `--check`
  validates and reports whether the committed header is current. It refuses to
  write when validation fails.
- `validate_values.py`: schema and load-time checks for the roster against
  `param-reference.json` and the live parameter manifest (clamping, inert
  story metadata, reliance on later automation).
- `check_story.py`: checks that literal claims in preset names, such as
  `Orbit` or `Whole-Note`, are true immediately after load, and that presets
  with a concrete recipe story (matched by name) still store that recipe.

## CI

`.github/workflows/ci.yml` runs on push to `main`, on pull requests, and on
manual dispatch. It checks C++ formatting (`format_sources.sh --check` with
clang-format 23.1.0), checks that the generated factory preset header is
current (`gen_factory.py --check`), runs the realtime-safety audit
(`audit_rt_safety.sh`) and the parser fixtures
(`test_headless_suite_parsing.sh`). On pull requests and manual dispatch only,
a macOS job builds the Release headless runner and runs `run_tests.sh
--no-build`.
`.github/workflows/release.yml` runs
only on manual dispatch (`windows-check` or `release`) and builds, validates,
and packages both platforms.
