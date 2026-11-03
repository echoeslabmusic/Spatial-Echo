# Contributing

Thanks for your interest in Spatial Echo.

## Reporting issues

Open a GitHub issue with:

- Your OS and host (name and version), or "standalone"
- Plug-in format (AU, VST3)
- Steps to reproduce, and a preset name if one is involved
- Expected and actual behaviour; a short audio example helps for sound issues

## Pull requests

1. Fork the repository and branch from `main`.
2. Build and run the relevant headless tests locally (see below).
3. Run `scripts/format_sources.sh` before committing; CI checks formatting.
4. Add or extend a test for new DSP, parameter or state behaviour.
5. Keep the factory preset header current: run
   `python3 tools/presets/gen_factory.py` after editing the preset roster.
6. Open a pull request that says what changed and why.

## Build and test loop

```bash
./scripts/run_tests.sh                    # build the Release runner, run the default suite
./scripts/run_test.sh <TestName>          # one test
./scripts/run_tests.sh --tests A,B --no-build
```

The default suite runs in parallel and takes a few minutes; `--debug` runs it
on the Debug runner with assertions enabled. `ctest --preset
macos-release-headless` runs the same tests after a build. Tests live under
`plugin/Source/Tests/<area>/` and each one is a single entry, with its kind
(`test`, `measurement` or `fixture`), in the registry in
`plugin/Source/Tests/Headless/HeadlessTestRunner.cpp`. Every test must emit a
pass or fail result; the suite treats an untagged or missing result as a
failure. Tests write files only under `artifactDirectory()` from
`plugin/Source/Tests/Common/TestUtilities.h`. Use a Release build for any
CPU, aliasing or sound-quality measurement; Debug timings are not evidence.
[docs/build-and-test.md](docs/build-and-test.md) has the
complete tool reference, including Windows presets, installers and validators.

## Audio-thread rules

Code that runs in `processBlock` or anything it calls must not:

- allocate or free memory
- lock a mutex or block on another thread
- log, print, or perform file or network I/O
- call host notification APIs

Use `std::atomic` or lock-free structures for cross-thread data and smoothed
values for parameter changes. `scripts/audit_rt_safety.sh` performs a static
scan of the hot paths, including `plugin/Source/DSP/dependencies/`, and runs
in CI.

## Code style

- C++17, JUCE conventions, Allman braces, 4-space indent, 120 columns
- `.clang-format` is the reference; `scripts/format_sources.sh` applies it to
  first-party sources and `scripts/format_sources.sh --check` verifies them
- Vendored third-party code (the `CloudSeedCore`, `chowdsp_wdf`,
  `saf_utilities` and `sofa` folders under `plugin/Source/DSP/dependencies/`)
  and the generated `FactoryPresetLibrary.h` are not reformatted; the
  first-party files directly inside `dependencies/` follow the project style
- Comments explain why, not what. Keep measurements and rationale; leave out
  history.

## Factory presets

Factory presets are generated. Edit `tools/presets/roster-values.json`, run
`python3 tools/presets/gen_factory.py`, and commit the regenerated
`plugin/Source/Presets/FactoryPresetLibrary.h` together with the roster.
`--check` reports whether the header is current without writing it. A new
preset also needs a browse-group entry in
`plugin/Source/Presets/FactoryPresetBrowseGroups.h` and a description in
`plugin/Source/Presets/FactoryPresetDescriptions.h`.
[docs/preset-system.md](docs/preset-system.md) describes
the schema and load semantics.

## Licence

By contributing you agree that your contributions are licensed under the
[MIT License](LICENSE). The Spatial Echo name and Echoes Lab logos are
trademarks and stay with Echoes Lab.
